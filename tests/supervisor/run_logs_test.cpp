// SPDX-License-Identifier: GPL-3.0-or-later
// The supervisor opening its logs as it starts (specs/logs/spec.md
// requirements 12, 13, and 15). Each test stops the supervisor at a starting
// failure, an engine that doesn't exist, so nothing here needs a socket or a
// child process. The acceptance tests cover a supervisor that runs.
#include <ks/supervisor/log_dir.h>
#include <ks/supervisor/options.h>
#include <ks/supervisor/run.h>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

using ks::supervisor::kExitOk;
using ks::supervisor::kExitStartFailed;
using ks::supervisor::Options;
using ks::supervisor::parse_session_name;
using ks::supervisor::SessionName;

/// A directory of the test's own, removed with everything in it.
class TempDir {
public:
    TempDir() {
        std::string name = (fs::temp_directory_path() / "ks-run-logs-XXXXXX").string();
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

/// Options for a supervisor in `dir` whose engine doesn't exist.
Options options_in(const fs::path& dir) {
    Options options;
    options.runtime_dir = dir / "run";
    options.log_dir = dir / "logs";
    options.engine = dir / "no-such-engine";
    return options;
}

/// Runs the supervisor, then puts back the signal dispositions run() changed.
int run_once(const Options& options, std::ostream& out, std::ostream& err) {
    const int status = ks::supervisor::run(options, out, err);
    constexpr std::array<int, 4> kSignals{SIGCHLD, SIGINT, SIGTERM, SIGHUP};
    for (const int signal : kSignals) {
        static_cast<void>(std::signal(signal, SIG_DFL));
    }
    return status;
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in{text};
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> lines_in(const fs::path& file) {
    const std::ifstream in{file};
    std::ostringstream text;
    text << in.rdbuf();
    return lines_of(text.str());
}

std::vector<fs::path> entries(const fs::path& folder) {
    std::vector<fs::path> result;
    for (const fs::directory_entry& entry : fs::directory_iterator{folder}) {
        result.push_back(entry.path());
    }
    return result;
}

unsigned mode_of(const fs::path& path) {
    struct stat status{};
    REQUIRE(::stat(path.c_str(), &status) == 0);
    return static_cast<unsigned>(status.st_mode) & 0777U;
}

} // namespace

TEST_CASE("Supervisor logs: a starting failure is in kool_spin.log and on standard error",
          "[supervisor][logs]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());
    const Options options = options_in(dir.path());
    std::ostringstream out;
    std::ostringstream err;

    CHECK(run_once(options, out, err) == kExitStartFailed);

    const std::vector<fs::path> sessions = entries(dir.path() / "logs");
    REQUIRE(sessions.size() == 1);
    const fs::path& session = sessions.front();
    const std::string name = session.filename().string();
    const std::optional<SessionName> parsed = parse_session_name(name);
    REQUIRE(parsed.has_value());
    CHECK(parsed.value_or(SessionName{}).pid == static_cast<std::uint64_t>(::getpid()));
    CHECK(mode_of(session) == 0700U);
    CHECK(mode_of(session / "kool_spin.log") == 0600U);

    const std::vector<std::string> log = lines_in(session / "kool_spin.log");
    REQUIRE(log.size() == 2);
    CHECK(log[0].starts_with("# kool_spin log; session started "));
    CHECK(log[1].find(" error can't start ") != std::string::npos);
    CHECK(log[1].find("no-such-engine") != std::string::npos);

    // ks-engine.log is created just before the engine is started, with the
    // session's clock.
    const std::vector<std::string> engine = lines_in(session / "ks-engine.log");
    REQUIRE(engine.size() == 1);
    CHECK(engine[0].starts_with("# ks-engine log; session started "));
    CHECK(engine[0].substr(engine[0].find(';')) == log[0].substr(log[0].find(';')));

    // The error is on standard error too, and nothing else is.
    const std::vector<std::string> reported = lines_of(err.str());
    REQUIRE(reported.size() == 1);
    CHECK(reported[0].starts_with("kool_spin: can't start "));
    CHECK(out.str().empty());
}

TEST_CASE("Supervisor logs: without a log folder, lines go to standard error",
          "[supervisor][logs]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());
    Options options = options_in(dir.path());
    const fs::path not_a_folder = dir.path() / "not-a-folder";
    {
        std::ofstream file{not_a_folder};
        file << "a file\n";
    }
    options.log_dir = not_a_folder;
    std::ostringstream out;
    std::ostringstream err;

    CHECK(run_once(options, out, err) == kExitStartFailed);

    // One warning saying why, with the path, then the starting failure.
    const std::vector<std::string> reported = lines_of(err.str());
    REQUIRE(reported.size() == 2);
    CHECK(reported[0].starts_with("kool_spin: can't create the log folder "));
    CHECK(reported[0].find(not_a_folder.string()) != std::string::npos);
    CHECK(reported[1].starts_with("kool_spin: can't start "));
    CHECK(fs::is_regular_file(not_a_folder));
    CHECK(lines_in(not_a_folder) == std::vector<std::string>{"a file"});
}

TEST_CASE("Supervisor logs: a session folder that can't be created", "[supervisor][logs]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());
    const Options options = options_in(dir.path());
    fs::create_directory(dir.path() / "logs");
    REQUIRE(::chmod((dir.path() / "logs").c_str(), 0500) == 0);
    std::ostringstream out;
    std::ostringstream err;

    const int status = run_once(options, out, err);
    REQUIRE(::chmod((dir.path() / "logs").c_str(), 0700) == 0);
    CHECK(status == kExitStartFailed);

    if (::geteuid() != 0) {
        const std::vector<std::string> reported = lines_of(err.str());
        REQUIRE(reported.size() == 2);
        CHECK(reported[0].starts_with("kool_spin: can't create the session folder "));
        CHECK(reported[0].find((dir.path() / "logs").string()) != std::string::npos);
        CHECK(reported[1].starts_with("kool_spin: can't start "));
        CHECK(entries(dir.path() / "logs").empty());
    }
}

TEST_CASE("Supervisor logs: no session folder before the log is open", "[supervisor][logs]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());
    Options options = options_in(dir.path());
    std::ostringstream out;
    std::ostringstream err;

    SECTION("--help") {
        options.help = true;
        CHECK(run_once(options, out, err) == kExitOk);
        CHECK(err.str().empty());
    }

    SECTION("a runtime directory that can't be used") {
        const fs::path file = dir.path() / "a-file";
        {
            std::ofstream made{file};
            made << "x\n";
        }
        options.runtime_dir = file;
        CHECK(run_once(options, out, err) == kExitStartFailed);
        CHECK(lines_of(err.str()).size() == 1);
    }

    CHECK(!fs::exists(dir.path() / "logs"));
}
