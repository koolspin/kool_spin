// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/supervisor/log_dir.h>

#include <ks/ipc/log.h>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

using ks::ipc::LogClock;
using ks::supervisor::create_log_dir;
using ks::supervisor::default_log_dir;
using ks::supervisor::parse_session_name;
using ks::supervisor::remove_old_sessions;
using ks::supervisor::session_name;
using ks::supervisor::SessionName;

constexpr std::int64_t kSecond = 1'000'000'000;

/// 2026-11-15 23:22:03 UTC, which is 18:22:03 at UTC-5.
constexpr std::int64_t kSystemSeconds = 1'794'784'923;

/// A directory of the test's own, removed with everything in it.
class TempDir {
public:
    TempDir() {
        std::string name = (fs::temp_directory_path() / "ks-log-dir-XXXXXX").string();
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
            fs::remove_all(path_, ignored);
        }
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void touch(const fs::path& path) {
    std::ofstream out{path};
    out << "x\n";
    REQUIRE(out.good());
}

/// The names in a folder, sorted.
std::vector<std::string> names(const fs::path& path) {
    std::vector<std::string> result;
    for (const fs::directory_entry& entry : fs::directory_iterator{path}) {
        result.push_back(entry.path().filename().string());
    }
    std::ranges::sort(result);
    return result;
}

unsigned mode_of(const fs::path& path) {
    struct stat status{};
    REQUIRE(::lstat(path.c_str(), &status) == 0);
    return static_cast<unsigned>(status.st_mode) & 07777U;
}

} // namespace

TEST_CASE("Log folder: the default", "[supervisor][log_dir]") {
    using Path = std::optional<fs::path>;
#ifdef __APPLE__
    CHECK(default_log_dir("/Users/dj", std::nullopt) == Path{"/Users/dj/Library/Logs/kool_spin"});
    CHECK(default_log_dir("/Users/dj", "/state") == Path{"/Users/dj/Library/Logs/kool_spin"});
    CHECK(default_log_dir(std::nullopt, "/state") == std::nullopt);
#else
    CHECK(default_log_dir("/home/dj", "/state") == Path{"/state/kool_spin/logs"});
    CHECK(default_log_dir(std::nullopt, "/state") == Path{"/state/kool_spin/logs"});
    CHECK(default_log_dir("/home/dj", std::nullopt) ==
          Path{"/home/dj/.local/state/kool_spin/logs"});
    // An XDG_STATE_HOME that isn't an absolute path is ignored.
    CHECK(default_log_dir("/home/dj", "state") == Path{"/home/dj/.local/state/kool_spin/logs"});
    CHECK(default_log_dir("/home/dj", "") == Path{"/home/dj/.local/state/kool_spin/logs"});
    CHECK(default_log_dir(std::nullopt, "state") == std::nullopt);
#endif
    // No usable HOME: no folder.
    CHECK(default_log_dir(std::nullopt, std::nullopt) == std::nullopt);
    CHECK(default_log_dir("", std::nullopt) == std::nullopt);
    CHECK(default_log_dir("home/dj", std::nullopt) == std::nullopt);
}

TEST_CASE("Log folder: created with its parents", "[supervisor][log_dir]") {
    const TempDir temp;
    REQUIRE(!temp.path().empty());

    const fs::path folder = temp.path() / "a" / "b" / "logs";
    CHECK(create_log_dir(folder) == std::error_code{});
    CHECK(fs::is_directory(folder));
    CHECK(mode_of(temp.path() / "a") == 0700U);
    CHECK(mode_of(temp.path() / "a" / "b") == 0700U);
    CHECK(mode_of(folder) == 0700U);

    SECTION("a folder that exists is left as it is") {
        REQUIRE(::chmod(folder.c_str(), 0750) == 0);
        touch(folder / "kept");
        CHECK(create_log_dir(folder) == std::error_code{});
        CHECK(mode_of(folder) == 0750U);
        CHECK(fs::exists(folder / "kept"));
    }
    SECTION("a trailing slash") {
        CHECK(create_log_dir(temp.path() / "c" / "d" / "") == std::error_code{});
        CHECK(fs::is_directory(temp.path() / "c" / "d"));
    }
    SECTION("a file in the way") {
        touch(temp.path() / "file");
        CHECK(create_log_dir(temp.path() / "file") != std::error_code{});
        CHECK(create_log_dir(temp.path() / "file" / "logs") != std::error_code{});
        CHECK(fs::is_regular_file(temp.path() / "file"));
    }
    SECTION("a link to a folder will do") {
        fs::create_directory_symlink(folder, temp.path() / "link");
        CHECK(create_log_dir(temp.path() / "link") == std::error_code{});
        CHECK(create_log_dir(temp.path() / "link" / "more") == std::error_code{});
        CHECK(fs::is_directory(folder / "more"));
    }
}

TEST_CASE("Log folder: a session's name", "[supervisor][log_dir]") {
    const LogClock clock{.steady_ns = 5,
                         .system_ns = (kSystemSeconds * kSecond) + 999'999'999,
                         .utc_offset_s = -18'000};
    CHECK(session_name(clock, 4321) == std::optional<std::string>{"2026-11-15_18-22-03_4321"});

    // The date is the local one.
    const LogClock ahead{
        .steady_ns = 5, .system_ns = kSystemSeconds * kSecond, .utc_offset_s = 3'600};
    CHECK(session_name(ahead, 1) == std::optional<std::string>{"2026-11-16_00-22-03_1"});

    // What it makes, it parses.
    const std::string name = session_name(clock, 4321).value_or("");
    const std::optional<SessionName> parsed = parse_session_name(name);
    REQUIRE(parsed.has_value());
    const SessionName parts = parsed.value_or(SessionName{});
    CHECK(parts.time == "2026-11-15_18-22-03");
    CHECK(parts.pid == 4321);

    SECTION("the years 1970 to 9999 only") {
        const LogClock epoch{.steady_ns = 0, .system_ns = 0, .utc_offset_s = 0};
        CHECK(session_name(epoch, 7) == std::optional<std::string>{"1970-01-01_00-00-00_7"});
        const LogClock before{.steady_ns = 0, .system_ns = -1, .utc_offset_s = 0};
        CHECK(session_name(before, 7) == std::nullopt);
        // Local time decides: one hour west of 1970's first second is 1969.
        const LogClock west{.steady_ns = 0, .system_ns = 0, .utc_offset_s = -3'600};
        CHECK(session_name(west, 7) == std::nullopt);
        const LogClock east{.steady_ns = 0, .system_ns = -1, .utc_offset_s = 3'600};
        CHECK(session_name(east, 7) == std::optional<std::string>{"1970-01-01_00-59-59_7"});

        const std::int64_t last = 253'402'300'799; // 9999-12-31 23:59:59
        const LogClock late{.steady_ns = 0, .system_ns = INT64_MAX, .utc_offset_s = 0};
        CHECK(session_name(late, 7) == std::optional<std::string>{"2262-04-11_23-47-16_7"});
        const LogClock end{.steady_ns = 0, .system_ns = 0, .utc_offset_s = last};
        CHECK(session_name(end, 7) == std::optional<std::string>{"9999-12-31_23-59-59_7"});
        const LogClock after{.steady_ns = 0, .system_ns = 0, .utc_offset_s = last + 1};
        CHECK(session_name(after, 7) == std::nullopt);
    }
    SECTION("offsets that would overflow") {
        const LogClock high{.steady_ns = 0, .system_ns = INT64_MAX, .utc_offset_s = INT64_MAX};
        CHECK(session_name(high, 7) == std::nullopt);
        const LogClock low{.steady_ns = 0, .system_ns = INT64_MIN, .utc_offset_s = INT64_MIN};
        CHECK(session_name(low, 7) == std::nullopt);
    }
    SECTION("a negative process ID") {
        CHECK(session_name(clock, -1) == std::nullopt);
    }
}

TEST_CASE("Log folder: which names are sessions", "[supervisor][log_dir]") {
    const std::optional<SessionName> parsed = parse_session_name("2020-01-02_03-04-05_678");
    REQUIRE(parsed.has_value());
    const SessionName parts = parsed.value_or(SessionName{});
    CHECK(parts.time == "2020-01-02_03-04-05");
    CHECK(parts.pid == 678);

    CHECK(parse_session_name("2020-01-02_03-04-05_0"));
    CHECK(parse_session_name("2020-01-02_03-04-05_9999999999999999999")); // 19 digits

    for (const std::string_view name : {
             "",
             "notes",
             "2020-01-02_03-04-05",
             "2020-01-02_03-04-05_",
             "2020-01-02_03-04-05_1.log",
             "2020-01-02_03-04-05_1 ",
             " 2020-01-02_03-04-05_1",
             "2020-01-02_03-04-05_-1",
             "2020-01-02_03-04-05_+1",
             "2020-01-02_03-04-05_0x1",
             "2020-01-02_03-04-05_12345678901234567890", // 20 digits
             "2020-01-02 03-04-05_1",
             "2020-01-02_03:04:05_1",
             "2020_01_02_03-04-05_1",
             "20a0-01-02_03-04-05_1",
             "2020-01-02_03-04-05-1",
             "020-01-02_03-04-05_1",
         }) {
        CAPTURE(name);
        CHECK_FALSE(parse_session_name(name));
    }
}

TEST_CASE("Log folder: keeps the newest sessions", "[supervisor][log_dir]") {
    const TempDir temp;
    REQUIRE(!temp.path().empty());
    const fs::path& logs = temp.path();

    const auto session = [&logs](int day) {
        const fs::path folder = logs / std::format("2020-01-{:02}_00-00-00_100", day);
        fs::create_directory(folder);
        touch(folder / "kool_spin.log");
        touch(folder / "ks-engine.log");
        return folder;
    };

    SECTION("the oldest go, and only their .log files") {
        std::vector<fs::path> folders;
        for (int day = 1; day <= 12; ++day) {
            folders.push_back(session(day));
        }
        // Things that aren't sessions, or aren't log files.
        fs::create_directory(logs / "notes");
        touch(logs / "notes" / "old.log");
        touch(logs / "2020-01-01_00-00-00_1.log");
        touch(logs / "2019-01-01_00-00-00_5"); // A file with a session's name.
        touch(folders[1] / "readme.txt");
        fs::create_directory(folders[2] / "sub.log"); // A folder named like a log.
        touch(folders[2] / "sub.log" / "inner.log");

        remove_old_sessions(logs);

        CHECK(names(logs) == std::vector<std::string>{
                                 "2019-01-01_00-00-00_5",
                                 "2020-01-01_00-00-00_1.log",
                                 "2020-01-02_00-00-00_100",
                                 "2020-01-03_00-00-00_100",
                                 "2020-01-04_00-00-00_100",
                                 "2020-01-05_00-00-00_100",
                                 "2020-01-06_00-00-00_100",
                                 "2020-01-07_00-00-00_100",
                                 "2020-01-08_00-00-00_100",
                                 "2020-01-09_00-00-00_100",
                                 "2020-01-10_00-00-00_100",
                                 "2020-01-11_00-00-00_100",
                                 "2020-01-12_00-00-00_100",
                                 "notes",
                             });
        CHECK(names(folders[1]) == std::vector<std::string>{"readme.txt"});
        CHECK(names(folders[2]) == std::vector<std::string>{"sub.log"});
        CHECK(fs::exists(folders[2] / "sub.log" / "inner.log"));
        CHECK(fs::exists(logs / "notes" / "old.log"));
        CHECK(names(folders[3]) == std::vector<std::string>{"kool_spin.log", "ks-engine.log"});
    }
    SECTION("few enough are all kept") {
        for (int day = 1; day <= 9; ++day) {
            session(day);
        }
        remove_old_sessions(logs);
        CHECK(names(logs).size() == 9);
        CHECK(names(logs / "2020-01-01_00-00-00_100").size() == 2);
    }
    SECTION("by time, then by process ID as a number") {
        for (const std::string_view name :
             {"2020-01-01_00-00-00_9", "2020-01-01_00-00-00_10", "2020-01-01_00-00-00_200",
              "2020-01-01_00-00-01_1", "2019-12-31_23-59-59_99999"}) {
            fs::create_directory(logs / name);
        }
        remove_old_sessions(logs, 3);
        CHECK(names(logs) == std::vector<std::string>{"2020-01-01_00-00-00_10",
                                                      "2020-01-01_00-00-00_200",
                                                      "2020-01-01_00-00-01_1"});
    }
    SECTION("keeping none") {
        session(1);
        session(2);
        remove_old_sessions(logs, 0);
        CHECK(names(logs).empty());
    }
    SECTION("links aren't followed") {
        // What the links point to, outside the log folder.
        const TempDir outside;
        REQUIRE(!outside.path().empty());
        const fs::path target = outside.path() / "2018-01-01_00-00-00_1";
        fs::create_directory(target);
        touch(target / "precious.log");
        touch(outside.path() / "file.log");

        // A link named like a session isn't one.
        fs::create_directory_symlink(target, logs / "2018-01-01_00-00-00_1");
        // A link named like a log file, in a session being removed.
        const fs::path old = session(1);
        fs::create_symlink(outside.path() / "file.log", old / "link.log");
        fs::create_directory_symlink(target, old / "folder.log");
        session(2);

        remove_old_sessions(logs, 1);

        CHECK(fs::exists(target / "precious.log"));
        CHECK(fs::exists(outside.path() / "file.log"));
        CHECK(fs::is_symlink(logs / "2018-01-01_00-00-00_1"));
        CHECK(names(old) == std::vector<std::string>{"folder.log", "link.log"});
        CHECK(fs::is_directory(logs / "2020-01-02_00-00-00_100"));
    }
    SECTION("a folder that isn't there") {
        remove_old_sessions(logs / "missing");
        CHECK(names(logs).empty());
    }
}
