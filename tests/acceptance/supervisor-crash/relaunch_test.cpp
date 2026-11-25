// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/supervisor-crash/spec.md: launching kool_spin
// while an engine from an earlier session runs (requirements 5 to 13).

#include <ks/ipc/messages.h>
#include <ks/ipc/recording.h>
#include <ks/ipc/runtime_dir.h>
#include <ks/ipc/state.h>

#include "relaunch_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace ks::supervisor_crash::test;
namespace msg = ks::ipc::msg;
namespace restart = ks::engine_restart::test;
using ks::processes::test::lock_holder;

constexpr std::string_view kQuestion = "Stop it and start a new session? [y/N] ";

std::string process_text(pid_t pid) {
    return "(process " + std::to_string(pid) + ")";
}

/// The session folders in `dir`'s log folder.
std::set<std::filesystem::path> sessions(const std::filesystem::path& dir) {
    const std::vector<std::filesystem::path> folders =
        ks::logs::test::session_folders(dir / "logs");
    return {folders.begin(), folders.end()};
}

/// True if `kool_spin.log` of the newest session but `before` has a line with
/// `text` at `level`.
bool new_log_says(const std::filesystem::path& dir, const std::set<std::filesystem::path>& before,
                  std::string_view level, const std::string& text) {
    for (const std::filesystem::path& folder : sessions(dir)) {
        if (!before.contains(folder)) {
            return ks::logs::test::wait_for_line(folder / "kool_spin.log", level, text).has_value();
        }
    }
    return false;
}

/// Stops `dir`'s kool_spin through a client.
bool stop(const std::filesystem::path& dir) {
    std::expected<restart::Client, std::string> client = restart::join(dir, "stopper");
    return client && restart::ask(*client, msg::Stop{}).has_value();
}

} // namespace

TEST_CASE("Relaunch: without a terminal, the engine is left running", "[crash][relaunch]") {
    const Files files;
    auto orphan = orphaned(files);
    const std::filesystem::path dir = orphan->session.dir();
    const pid_t engine = orphan->session.engine();
    REQUIRE(engine > 0);
    const auto before = sessions(dir);
    REQUIRE(before.size() == 1);

    for (int i = 0; i < 12; ++i) {
        Launch launch{files.path(), launch_args(dir, {})};
        REQUIRE(launch.process().wait(kLaunchWait) == 1);
        CAPTURE(launch.err());
        CHECK(launch.err().find(process_text(engine)) != std::string::npos);
        CHECK(launch.err().find("--stop-previous") != std::string::npos);
        CHECK(launch.err().find(kQuestion) == std::string::npos);
    }
    CHECK(sessions(dir) == before);
    CHECK(orphan->session.engine() == engine);
    CHECK(keeps_beating(orphan->client.state));
    CHECK_FALSE(lock_holder(dir, ks::ipc::kSupervisorLockName).has_value());
}

TEST_CASE("Relaunch: --stop-previous stops it and starts a session", "[crash][relaunch]") {
    const Files files;

    SECTION("with an engine from an earlier session") {
        auto orphan = orphaned(files);
        const std::filesystem::path dir = orphan->session.dir();
        const pid_t engine = orphan->session.engine();
        REQUIRE(engine > 0);
        const auto before = sessions(dir);

        Launch launch{files.path(), launch_args(dir, {"--stop-previous"})};
        REQUIRE(new_session(dir, engine));
        CAPTURE(launch.err());
        CHECK(launch.process().running());
        CHECK(ended(engine));

        // The old engine faded out: its last block says so, and its recording
        // falls to silence without a step.
        const auto status = restart::require(orphan->client.state.status());
        constexpr std::uint64_t kBoth = ks::ipc::kStatusMuted | ks::ipc::kStatusSilenced;
        CHECK((status.flags & kBoth) == kBoth);
        const std::uint64_t written = orphan->recording.reader().frames_written();
        REQUIRE(written > 20'000);
        const auto end = restart::require(orphan->recording.left(written - 20'000, 20'000));
        CHECK(std::abs(end.back()) < 1e-4F);
        float highest = 0.0F;
        float widest = 0.0F;
        for (std::size_t i = 1; i < end.size(); ++i) {
            highest = std::max(highest, end[i]);
            widest = std::max(widest, std::abs(end[i] - end[i - 1]));
        }
        CAPTURE(highest, widest);
        CHECK(highest > 0.1F);
        CHECK(widest < 0.01F * highest);

        CHECK(launch.err().find("stopped the engine from an earlier session " +
                                process_text(engine)) != std::string::npos);
        CHECK(new_log_says(dir, before, "info",
                           "stopped the engine from an earlier session " + process_text(engine)));

        // A session like any other.
        std::expected<restart::Client, std::string> client = restart::join(dir, "after");
        REQUIRE(client.has_value());
        const std::filesystem::path wav = restart::write_constant(files.path(), "more.wav", 5);
        CHECK(restart::is_loaded(restart::load(*client, 0, wav)));
        CHECK(stop(dir));
        CHECK(launch.process().wait(kLaunchWait) == 0);
    }

    SECTION("with nothing to stop") {
        const ks::ipc::test::TempDir dir;
        const ks::processes::test::EngineReaper reaper{dir.path()};
        Launch launch{files.path(), launch_args(dir.path(), {"--stop-previous"})};
        REQUIRE(new_session(dir.path(), 0));
        CHECK(launch.err().empty());
        CHECK(stop(dir.path()));
        CHECK(launch.process().wait(kLaunchWait) == 0);
    }
}

TEST_CASE("Relaunch: the prompt", "[crash][relaunch]") {
    const Files files;
    auto orphan = orphaned(files);
    const std::filesystem::path dir = orphan->session.dir();
    const pid_t first = orphan->session.engine();
    REQUIRE(first > 0);
    const auto before = sessions(dir);

    SECTION("no") {
        // Control-D is end of file, and Control-C is SIGINT.
        const std::string too_long(100, 'y');
        for (const std::string& answer :
             {std::string{"\n"}, std::string{"n\n"}, std::string{"yess\n"},
              "y" + std::string(80, ' ') + "\n", too_long + "\n", std::string{"\x04"},
              std::string{"y\x04\x04"}, std::string{"\x03"}}) {
            CAPTURE(answer);
            TerminalLaunch launch{launch_args(dir, {})};
            REQUIRE(launch.shows(kQuestion));
            CHECK(launch.shown().find(process_text(first)) != std::string::npos);
            launch.type(answer);
            const std::optional<int> status = launch.wait();
            CAPTURE(launch.shown());
            CHECK(status == (answer == "\x03" ? 128 + SIGINT : 1));
            CHECK_FALSE(lock_holder(dir, ks::ipc::kSupervisorLockName).has_value());
        }
        CHECK(orphan->session.engine() == first);
        CHECK(keeps_beating(orphan->client.state));
        CHECK(sessions(dir) == before);
    }

    SECTION("yes") {
        pid_t old = first;
        for (const std::string_view answer : {"y\n", "YES\n", " yes \n"}) {
            CAPTURE(answer);
            TerminalLaunch launch{launch_args(dir, {})};
            REQUIRE(launch.shows(kQuestion));
            launch.type(answer);
            REQUIRE(new_session(dir, old));
            CHECK(ended(old));
            old = engine_of(dir);
            // That session crashes too.
            kill_supervisor(dir);
            CHECK(launch.wait() == 128 + SIGKILL);
            CHECK(engine_of(dir) == old);
        }
    }
}

TEST_CASE("Relaunch: a second launch while one asks", "[crash][relaunch]") {
    const Files files;
    auto orphan = orphaned(files);
    const std::filesystem::path dir = orphan->session.dir();
    TerminalLaunch asking{launch_args(dir, {})};
    REQUIRE(asking.shows(kQuestion));
    Launch second{files.path(), launch_args(dir, {"--stop-previous"})};
    CHECK(second.process().wait(kLaunchWait) == 1);
    CHECK(second.err().find("another supervisor is running") != std::string::npos);
    CHECK(keeps_beating(orphan->client.state));
    asking.type("\n");
    CHECK(asking.wait() == 1);
}

TEST_CASE("Relaunch: a background launch doesn't ask", "[crash][relaunch]") {
    const Files files;
    auto orphan = orphaned(files);
    const std::filesystem::path dir = orphan->session.dir();
    TerminalLaunch launch{launch_args(dir, {}), "background"};
    CHECK(launch.wait() == 1);
    CAPTURE(launch.shown());
    CHECK(launch.shown().find("--stop-previous") != std::string::npos);
    CHECK(launch.shown().find(kQuestion) == std::string::npos);
    CHECK(keeps_beating(orphan->client.state));
}

TEST_CASE("Relaunch: a launch that can't start leaves the engine running", "[crash][relaunch]") {
    const Files files;
    auto orphan = orphaned(files);
    const std::filesystem::path dir = orphan->session.dir();
    const pid_t engine = orphan->session.engine();
    const std::string missing = (files.path() / "missing").string();
    const std::string not_executable = (files.path() / "constant.wav").string();

    for (const auto& [engine_path, worker_path] :
         {std::pair{std::string{ks::processes::test::kEnginePath}, missing},
          std::pair{missing, std::string{KS_WORKER_PATH}},
          std::pair{not_executable, std::string{KS_WORKER_PATH}}}) {
        CAPTURE(engine_path, worker_path);
        Launch launch{files.path(),
                      launch_args(dir, {"--stop-previous"}, engine_path, worker_path)};
        CHECK(launch.process().wait(kLaunchWait) == 1);
        CHECK(orphan->session.engine() == engine);
    }
    CHECK(keeps_beating(orphan->client.state));
}

TEST_CASE("Relaunch: a stuck engine is killed", "[crash][relaunch]") {
    const Files files;
    auto orphan = orphaned(files);
    const std::filesystem::path dir = orphan->session.dir();
    const pid_t engine = orphan->session.engine();
    const auto before = sessions(dir);
    REQUIRE(::kill(engine, SIGSTOP) == 0);

    const Clock::time_point began = Clock::now();
    Launch launch{files.path(), launch_args(dir, {"--stop-previous"})};
    REQUIRE(new_session(dir, engine));
    CHECK(Clock::now() - began < (kSlowSanitizer ? Milliseconds{10'000} : Milliseconds{5'000}));
    CHECK(ended(engine));
    CHECK(launch.err().find("was killed") != std::string::npos);
    CHECK(new_log_says(dir, before, "warning", "was killed"));
    CHECK(stop(dir));
    CHECK(launch.process().wait(kLaunchWait) == 0);
}

TEST_CASE("Relaunch: an unknown holder", "[crash][relaunch]") {
    const Files files;
    const ks::ipc::test::TempDir dir;
    Process holder = hold_lock(dir.path(), "stops");
    write_lock(dir.path(), "");
    // The probe writes a report if it's started at all.
    const std::filesystem::path report = dir.path() / "report";

    const auto refused = [&](const std::vector<std::string>& options) {
        std::vector<std::string> args =
            launch_args(dir.path(), options, ks::processes::test::kProbePath);
        args.insert(args.end(), {"--report", report.string()});
        Launch launch{files.path(), args};
        CHECK(launch.process().wait(kLaunchWait) == 1);
        CHECK(launch.err().find("still running") != std::string::npos);
    };
    SECTION("without a terminal") {
        refused({});
    }
    SECTION("with --stop-previous") {
        refused({"--stop-previous"});
    }
    SECTION("on a terminal") {
        TerminalLaunch launch{launch_args(dir.path(), {}, ks::processes::test::kProbePath)};
        CHECK(launch.wait() == 1);
        CHECK(launch.shown().find("still running") != std::string::npos);
        CHECK(launch.shown().find(kQuestion) == std::string::npos);
    }
    CHECK(holder.running());
    CHECK_FALSE(std::filesystem::exists(report));
    CHECK(sessions(dir.path()).empty());
}

TEST_CASE("Relaunch: options", "[crash][relaunch]") {
    const Files files;
    const ks::ipc::test::TempDir dir;
    Launch twice{files.path(), launch_args(dir.path(), {"--stop-previous", "--stop-previous"})};
    CHECK(twice.process().wait() == 2);

    const std::filesystem::path out = files.path() / "help";
    Process help =
        ks::processes::test::start("/bin/sh", {"-c", R"(exec "$1" --help >"$0")", out.string(),
                                               std::string{ks::processes::test::kKoolSpinPath}});
    CHECK(help.wait() == 0);
    bool listed = false;
    for (const std::string& line : ks::logs::test::read_lines(out)) {
        listed = listed || line.find("--stop-previous") != std::string::npos;
    }
    CHECK(listed);
}

TEST_CASE("Relaunch: nothing is left behind", "[crash][relaunch]") {
    const Files files;
    auto orphan = orphaned(files);
    const std::filesystem::path dir = orphan->session.dir();
    std::vector<pid_t> all{orphan->session.process().pid(), orphan->session.engine()};
    for (const auto& [track, worker] :
         ks::track_loading::test::worker_pids(orphan->session.kool_spin_log())) {
        static_cast<void>(track);
        all.push_back(worker);
    }
    {
        Launch refused{files.path(), launch_args(dir, {})};
        all.push_back(refused.process().pid());
        CHECK(refused.process().wait(kLaunchWait) == 1);
    }
    Launch launch{files.path(), launch_args(dir, {"--stop-previous"})};
    REQUIRE(new_session(dir, all[1]));
    all.push_back(launch.process().pid());
    all.push_back(engine_of(dir));
    CHECK(stop(dir));
    CHECK(launch.process().wait(kLaunchWait) == 0);

    for (const pid_t pid : all) {
        CAPTURE(pid);
        CHECK(eventually([&] { return ended(pid); }));
    }
    CHECK_FALSE(lock_holder(dir, ks::ipc::kEngineLockName).has_value());
    CHECK_FALSE(lock_holder(dir, ks::ipc::kSupervisorLockName).has_value());
    CHECK_FALSE(std::filesystem::exists(dir / "supervisor.sock"));
}
