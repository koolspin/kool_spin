// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/supervisor/log_file.h>

#include <ks/ipc/log.h>

#include <catch2/catch_test_macros.hpp>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using ks::ipc::LogClock;
using ks::ipc::LogLevel;
using ks::ipc::LogRecord;
using ks::supervisor::LogFile;

constexpr std::uint64_t kSecond = 1'000'000'000;

/// Steady time 1,000 s is 2026-11-15 18:22:03 at UTC-5.
constexpr LogClock kClock{.steady_ns = 1'000 * kSecond,
                          .system_ns = std::int64_t{1'794'784'923} * 1'000'000'000,
                          .utc_offset_s = -18'000};

/// A directory of the test's own, removed with everything in it.
class TempDir {
public:
    TempDir() {
        std::string name = (std::filesystem::temp_directory_path() / "ks-log-file-XXXXXX").string();
        if (::mkdtemp(name.data()) != nullptr) {
            path_ = name;
        }
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::vector<std::string> lines_of(const std::string& contents) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < contents.size()) {
        const std::size_t end = contents.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(contents.substr(start) + "<no newline>");
            break;
        }
        lines.push_back(contents.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

LogRecord make_record(std::uint64_t time, std::uint64_t level, std::string_view text,
                      std::uint64_t flags = 0) {
    LogRecord record{};
    record.time = time;
    record.level = level;
    record.length = text.size();
    record.flags = flags;
    std::ranges::copy(text, record.text.begin());
    return record;
}

/// Caps the size of the files this process writes, until destroyed. A write
/// past the cap fails with EFBIG, once SIGXFSZ is ignored.
class FileSizeCap {
public:
    explicit FileSizeCap(rlim_t bytes) {
        struct sigaction ignore{};
        ignore.sa_handler =
            SIG_IGN; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast): the macro casts.
        ok_ = ::sigaction(SIGXFSZ, &ignore, &old_action_) == 0 &&
              ::getrlimit(RLIMIT_FSIZE, &old_limit_) == 0;
        if (ok_) {
            rlimit capped = old_limit_;
            capped.rlim_cur = bytes;
            ok_ = ::setrlimit(RLIMIT_FSIZE, &capped) == 0;
        }
    }
    FileSizeCap(const FileSizeCap&) = delete;
    FileSizeCap& operator=(const FileSizeCap&) = delete;
    FileSizeCap(FileSizeCap&&) = delete;
    FileSizeCap& operator=(FileSizeCap&&) = delete;
    ~FileSizeCap() {
        static_cast<void>(::setrlimit(RLIMIT_FSIZE, &old_limit_));
        static_cast<void>(::sigaction(SIGXFSZ, &old_action_, nullptr));
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    struct sigaction old_action_{};
    rlimit old_limit_{};
    bool ok_ = false;
};

} // namespace

TEST_CASE("LogFile: create() writes the header, and only into a new file", "[supervisor][log]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());
    const std::filesystem::path path = dir.path() / "kool_spin.log";

    {
        const std::expected<LogFile, std::error_code> file =
            LogFile::create(path, "kool_spin", kClock);
        REQUIRE(file.has_value());
        CHECK(!file->error().has_value());
    }
    const std::string header = ks::ipc::format_log_header("kool_spin", kClock);
    CHECK(header == "# kool_spin log; session started 2026-11-15 18:22:03.000000 -05:00; "
                    "steady clock 1000.000000000");
    CHECK(read_file(path) == header + "\n");

    struct stat status{};
    REQUIRE(::stat(path.c_str(), &status) == 0);
    CHECK((status.st_mode & 07777U) == 0600U);

    const std::expected<LogFile, std::error_code> again =
        LogFile::create(path, "kool_spin", kClock);
    REQUIRE(!again.has_value());
    CHECK(again.error() == std::errc::file_exists);
    CHECK(read_file(path) == header + "\n");

    // A symbolic link is a path that exists, wherever it points.
    const std::filesystem::path link = dir.path() / "link.log";
    std::filesystem::create_symlink(dir.path() / "target.log", link);
    CHECK(!LogFile::create(link, "kool_spin", kClock).has_value());
    CHECK(!std::filesystem::exists(dir.path() / "target.log"));

    const std::expected<LogFile, std::error_code> nowhere =
        LogFile::create(dir.path() / "missing" / "x.log", "kool_spin", kClock);
    REQUIRE(!nowhere.has_value());
    CHECK(nowhere.error() == std::errc::no_such_file_or_directory);
}

TEST_CASE("LogFile: each write() is one formatted line, in the file at once", "[supervisor][log]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());
    const std::filesystem::path path = dir.path() / "ks-engine.log";
    std::expected<LogFile, std::error_code> file = LogFile::create(path, "ks-engine", kClock);
    REQUIRE(file.has_value());

    CHECK(file->write((1'000 * kSecond) + (kSecond / 2), LogLevel::Info, "hello"));
    // Unbuffered: the line is there while the file is still open.
    CHECK(lines_of(read_file(path)).size() == 2);

    CHECK(file->write(1'001 * kSecond, LogLevel::Error, "a\tb\nc"));
    CHECK(file->write(make_record(1'002 * kSecond, 2, "from a ring")));
    CHECK(file->write(make_record(1'003 * kSecond, 3, "cut", ks::ipc::kLogTruncated)));
    // An invalid record is still a line that was written.
    CHECK(file->write(make_record(1'004 * kSecond, 9, "bad level")));
    CHECK(!file->error().has_value());

    const std::vector<std::string> lines = lines_of(read_file(path));
    REQUIRE(lines.size() == 6);
    const std::span<const std::string> line{lines};
    CHECK(line[0] == ks::ipc::format_log_header("ks-engine", kClock));
    CHECK(line[1] == "2026-11-15 18:22:03.500000 info hello");
    CHECK(line[2] == "2026-11-15 18:22:04.000000 error a\\x09b\\x0ac");
    CHECK(line[3] == "2026-11-15 18:22:05.000000 warning from a ring");
    CHECK(line[4] == "2026-11-15 18:22:06.000000 info cut…");
    CHECK(line[5] == "2026-11-15 18:22:07.000000 warning [invalid log record]");
}

TEST_CASE("LogFile: the limit", "[supervisor][log]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());
    const std::filesystem::path path = dir.path() / "test.log";
    const std::size_t header_size = ks::ipc::format_log_header("test", kClock).size() + 1;
    const std::string first =
        ks::ipc::format_log_line(kClock, kClock.steady_ns, LogLevel::Info, "x");

    SECTION("a line that ends exactly at the limit is written") {
        const std::uint64_t limit = header_size + first.size() + 1;
        std::expected<LogFile, std::error_code> file = LogFile::create(path, "test", kClock, limit);
        REQUIRE(file.has_value());
        CHECK(file->write(kClock.steady_ns, LogLevel::Info, "x"));
        CHECK(std::filesystem::file_size(path) == limit);

        // The next one isn't: the warning takes its place, with its time.
        CHECK(!file->write(kClock.steady_ns + kSecond, LogLevel::Error, "y"));
        const std::vector<std::string> lines = lines_of(read_file(path));
        REQUIRE(lines.size() == 3);
        CHECK(lines.back().starts_with("2026-11-15 18:22:04.000000 warning "));
        CHECK(lines.back().contains("limit"));
        const std::uintmax_t size = std::filesystem::file_size(path);
        CHECK(size == limit + lines.back().size() + 1);

        // Nothing follows the warning, not even a second warning.
        CHECK(!file->write(kClock.steady_ns, LogLevel::Info, "x"));
        CHECK(!file->write(make_record(kClock.steady_ns, 1, "z")));
        CHECK(std::filesystem::file_size(path) == size);
        CHECK(!file->error().has_value());
    }

    SECTION("a line one byte too long isn't") {
        const std::uint64_t limit = header_size + first.size();
        std::expected<LogFile, std::error_code> file = LogFile::create(path, "test", kClock, limit);
        REQUIRE(file.has_value());
        CHECK(!file->write(kClock.steady_ns, LogLevel::Info, "x"));
        const std::vector<std::string> lines = lines_of(read_file(path));
        REQUIRE(lines.size() == 2);
        CHECK(lines.back().contains(" warning "));
    }

    SECTION("a limit the header alone passes") {
        std::expected<LogFile, std::error_code> file = LogFile::create(path, "test", kClock, 0);
        REQUIRE(file.has_value());
        CHECK(!file->write(make_record(kClock.steady_ns, 3, "x")));
        CHECK(!file->write(kClock.steady_ns, LogLevel::Info, "x"));
        CHECK(lines_of(read_file(path)).size() == 2);
        CHECK(!file->error().has_value());
    }
}

TEST_CASE("LogFile: a write error stops the file", "[supervisor][log]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());
    const std::filesystem::path path = dir.path() / "test.log";
    std::expected<LogFile, std::error_code> file = LogFile::create(path, "test", kClock);
    REQUIRE(file.has_value());

    // The file can't grow past 1,024 bytes: some lines fit, then one doesn't.
    constexpr int kLines = 100;
    std::array<bool, kLines> written{};
    std::optional<std::error_code> error;
    bool capped = false;
    {
        const FileSizeCap cap{1'024};
        capped = cap.ok();
        if (capped) {
            for (bool& result : written) {
                result = file->write(kClock.steady_ns, LogLevel::Info, "a line of some length");
            }
            error = file->error();
        }
    }
    REQUIRE(capped);

    const std::ptrdiff_t succeeded = std::ranges::find(written, false) - written.begin();
    CHECK(succeeded > 5);
    REQUIRE(succeeded < kLines);
    // Once one has failed, every later one does.
    CHECK(std::ranges::count(written, true) == succeeded);
    REQUIRE(error.has_value());
    CHECK(error.value_or(std::error_code{}) == std::errc::file_too_large);

    // With the cap gone, the file still isn't written.
    const std::uintmax_t size = std::filesystem::file_size(path);
    CHECK(size <= 1'024);
    CHECK(!file->write(kClock.steady_ns, LogLevel::Error, "later"));
    CHECK(!file->write(make_record(kClock.steady_ns, 1, "later")));
    CHECK(std::filesystem::file_size(path) == size);
    CHECK(file->error() == error);

    // Every whole line in it is one that write() reported as written.
    const std::string contents = read_file(path);
    const std::ptrdiff_t newlines = std::ranges::count(contents, '\n');
    CHECK(newlines == succeeded + 1);
}
