// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/state-folder/spec.md: the locks in the state
// folder, and which folder that is (requirements 2 and 5 to 11).

#include <ks/ipc/messages.h>
#include <ks/ipc/runtime_dir.h>

#include "state_support.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ks::state_folder::test;
using ks::ipc::LockFile;
namespace fs = std::filesystem;
namespace msg = ks::ipc::msg;
namespace restart = ks::engine_restart::test;

/// What kool_spin says when its state folder can't be used: a warning with
/// the folder's path, in its log and on standard error.
void check_warned(const restart::Session& session, const fs::path& state) {
    CAPTURE(session.stderr_text(), state);
    CHECK(has(session.stderr_text(), state.string()));
    CHECK(ks::logs::test::wait_for_line(session.kool_spin_log(), "warning", state.string())
              .has_value());
}

/// Kills the engine of `session`, whose deck 0 plays, and checks that a new
/// one plays on.
void check_restarts(Playing& session) {
    const auto [old, when] = restart::kill_engine(session.session);
    static_cast<void>(when);
    REQUIRE(session.session.engine_replaced(old));
    // Until the new engine renders, the old one's last blocks are still there.
    CHECK(eventually([&] { return keeps_beating(session.client.state); }));
    CHECK(playing_deck(session.client.state, 0).has_value());
}

/// ks-engine run directly, with the test as its supervisor and `state` as its
/// state folder, reporting to standard error.
struct DirectEngine {
    std::unique_ptr<ks::logs::test::LoggedEngine> engine;

    explicit DirectEngine(const fs::path& state) : engine{ks::logs::test::prepare_engine()} {
        engine->supervised->socket = ks::logs::test::launch_engine(
            *engine, ks::logs::test::engine_args(*engine, {"--state-dir", state.string()}));
    }

    [[nodiscard]] const fs::path& dir() const { return engine->supervised->dir.path(); }
    [[nodiscard]] Process& process() const { return engine->supervised->engine; }
    [[nodiscard]] bool ready() const {
        return ks::processes::test::is<msg::Ready>(
            ks::processes::test::receive(*engine->supervised->socket), 0);
    }
    [[nodiscard]] std::string err() const {
        std::string text;
        for (const std::string& line : ks::logs::test::read_lines(engine->err)) {
            text += line + "\n";
        }
        return text;
    }
};

} // namespace

TEST_CASE("State locks: both folders", "[state-folder][locks]") {
    const Folder state;
    restart::Session session{session_options(state.path())};
    const pid_t supervisor = session.process().pid();
    CHECK(held_by(session.dir(), kSupervisorLockName, supervisor));
    CHECK(held_by(state.path(), kSupervisorLockName, supervisor));
    REQUIRE(eventually([&] { return session.engine() > 0; }));
    CHECK(held_by(state.path(), kEngineLockName, session.engine()));

    // A client's Welcome comes after the engine's Ready, so a crash now
    // restarts it.
    REQUIRE(answers(session.dir()));
    const auto [old, when] = restart::kill_engine(session);
    static_cast<void>(when);
    REQUIRE(session.engine_replaced(old));
    CHECK(held_by(state.path(), kEngineLockName, session.engine()));
    CHECK(held_by(state.path(), kSupervisorLockName, supervisor));
    // Nothing to warn of.
    CHECK(!has(session.stderr_text(), state.path().string()));

    session.process().signal(SIGTERM);
    CHECK(session.process().wait(kLaunchWait) == 0);
    CHECK(all_free(session.dir()));
    CHECK(all_free(state.path()));
}

TEST_CASE("State locks: one folder for both", "[state-folder][locks]") {
    const Files files;
    const Folder dir;
    fs::path state = dir.path();
    SECTION("the same path") {}
    SECTION("through a symbolic link") {
        state = files.path() / "link";
        fs::create_directory_symlink(dir.path(), state);
    }
    CAPTURE(state);
    Launch launch{files.path(), launch_args(dir.path(), with_state(state))};
    REQUIRE(new_session(dir.path(), 0));
    CHECK(lock_holder(dir.path(), kSupervisorLockName) == launch.process().pid());
    CHECK(launch.err().empty());
    CHECK(stop(dir.path()));
    CHECK(launch.process().wait(kLaunchWait) == 0);
    CHECK(all_free(dir.path()));
}

TEST_CASE("State locks: a second supervisor", "[state-folder][locks]") {
    const Files files;
    const Folder state;
    restart::Session first{session_options(state.path())};
    const std::string process = process_text(first.process().pid());
    REQUIRE(answers(first.dir()));

    SECTION("the same state folder") {
        const Folder other;
        Launch second{files.path(), launch_args(other.path(), with_state(state.path()))};
        CHECK(second.process().wait(kLaunchWait) == 1);
        CAPTURE(second.err());
        CHECK(has(second.err(), process));
        CHECK(has(second.err(), "another runtime directory"));
        CHECK(all_free(other.path()));
    }

    SECTION("the same runtime directory") {
        const Folder other;
        Launch second{files.path(), launch_args(first.dir(), with_state(other.path()))};
        CHECK(second.process().wait(kLaunchWait) == 1);
        CAPTURE(second.err());
        CHECK(has(second.err(), process));
        CHECK(all_free(other.path()));
    }

    CHECK(first.process().running());
    CHECK(answers(first.dir()));
    CHECK(held_by(state.path(), kSupervisorLockName, first.process().pid()));
}

TEST_CASE("State locks: which folder", "[state-folder][locks]") {
    const Files files;
    // A home directory and the rest, with short names: on macOS a socket's
    // path must fit in 104 bytes.
    const Folder place;
    const fs::path home = place.path() / "h";
    const fs::path xdg_state = place.path() / "s";
    const fs::path run = place.path() / "r";
    for (const fs::path& folder : {home, xdg_state, run}) {
        REQUIRE(fs::create_directory(folder));
    }
    const std::vector<std::string> env{"HOME=" + home.string(),
                                       "XDG_STATE_HOME=" + xdg_state.string(),
                                       "XDG_RUNTIME_DIR=" + run.string(), "TMPDIR=" + run.string()};
    const std::vector<std::string> common{
        "--log-dir", (files.path() / "logs").string(),
        "--engine",  std::string{ks::processes::test::kEnginePath},
        "--worker",  KS_WORKER_PATH,
        "--",        "--backend",
        "null"};

    SECTION("--runtime-dir alone") {
        const Folder dir;
        std::vector<std::string> args{"--runtime-dir", dir.path().string()};
        args.insert(args.end(), common.begin(), common.end());
        Running kool_spin{files.path(), ks::processes::test::kKoolSpinPath, args, env};
        REQUIRE(new_session(dir.path(), 0));
        CHECK(stop(dir.path()));
        CHECK(kool_spin.process().wait(kLaunchWait) == 0);
        CHECK(kool_spin.out().empty());
        CHECK(files_in(home).empty());
        CHECK(files_in(xdg_state).empty());
        CHECK(files_in(run).empty());
    }

    SECTION("neither option") {
        const fs::path runtime = run / "kool_spin";
#ifdef __APPLE__
        const fs::path state = home / "Library/Application Support/kool_spin";
#else
        const fs::path state = xdg_state / "kool_spin";
#endif
        const ks::processes::test::EngineReaper runtime_reaper{runtime};
        const ks::processes::test::EngineReaper state_reaper{state};
        Running kool_spin{files.path(), ks::processes::test::kKoolSpinPath, common, env};
        REQUIRE(new_session(runtime, 0));
        const pid_t supervisor = kool_spin.process().pid();
        const pid_t engine = engine_of(runtime);
        CHECK(held_by(runtime, kSupervisorLockName, supervisor));
        CHECK(held_by(state, kSupervisorLockName, supervisor));
        CHECK(held_by(state, kEngineLockName, engine));
        CHECK(stop(runtime));
        CHECK(kool_spin.process().wait(kLaunchWait) == 0);
        CHECK(kool_spin.out().empty());
        CHECK(all_free(runtime));
        CHECK(all_free(state));
    }
}

TEST_CASE("State locks: a bad folder never stops the music", "[state-folder][locks]") {
    const Files files;
    const Folder place;
    const fs::path state = place.path() / "state";

    SECTION("a file") {
        std::ofstream{state} << "contents";
        const auto session = playing_with(files, state);
        check_warned(session->session, state);
        check_restarts(*session);
        CHECK(fs::file_size(state) == 8);
    }

    SECTION("one that others can write") {
        make_private(state);
        const Mode mode{state, 0777};
        const auto session = playing_with(files, state);
        check_warned(session->session, state);
        check_restarts(*session);
        CHECK(files_in(state).empty());
    }

    SECTION("one that can't be created") {
        skip_as_root();
        const Mode mode{place.path(), 0500};
        const auto session = playing_with(files, state);
        check_warned(session->session, state);
        check_restarts(*session);
    }

    SECTION("one that's refused by the time the engine restarts") {
        make_private(state);
        const auto session = playing_with(files, state);
        REQUIRE(held_by(state, kEngineLockName, session->session.engine()));
        const Mode mode{state, 0777};
        check_restarts(*session);
        CHECK(
            ks::logs::test::wait_for_line(session->session.engine_log(), "warning", state.string())
                .has_value());
    }

    SECTION("one that becomes unusable") {
        skip_as_root();
        make_private(state);
        // The state folder's engine lock is out of the reaper's reach while
        // its mode is 0: the session's own reaper ends the engine.
        const auto session = playing_with(files, state);
        REQUIRE(held_by(state, kEngineLockName, session->session.engine()));
        const Mode folder{state, 0500};
        const Mode file{lock_path(state, kEngineLockName), 0000};
        check_restarts(*session);
        CHECK(
            ks::logs::test::wait_for_line(session->session.engine_log(), "warning", state.string())
                .has_value());
    }
}

TEST_CASE("State locks: a lock that can't be written", "[state-folder][locks]") {
    const Folder state;
    // A lock file that's already there, as after any earlier session.
    REQUIRE(LockFile::acquire(state.path(), kEngineLockName).has_value());

    SECTION("a full disk") {
        // The limit on file sizes stands in for one: a write fails, with
        // EFBIG rather than ENOSPC.
        const pid_t pid = ::fork();
        REQUIRE(pid != -1);
        if (pid == 0) {
            static_cast<void>(std::signal(SIGXFSZ, SIG_IGN));
            const struct rlimit none{.rlim_cur = 0, .rlim_max = 0};
            if (::setrlimit(RLIMIT_FSIZE, &none) != 0) {
                ::_exit(2);
            }
            const std::expected<LockFile, LockFile::Failure> lock =
                LockFile::acquire(state.path(), kEngineLockName);
            if (lock || lock.error().error.value() != EFBIG || lock.error().holder) {
                ::_exit(3);
            }
            // It let go of the lock, although the process lives on.
            ::_exit(lock_holder(state.path(), kEngineLockName) ? 4 : 0);
        }
        ks::ipc::test::Child child{pid};
        CHECK(child.wait() == 0);
        CHECK(all_free(state.path()));
    }

    SECTION("a file without permission") {
        skip_as_root();
        const Mode mode{lock_path(state.path(), kEngineLockName), 0400};
        const DirectEngine engine{state.path()};
        REQUIRE(engine.ready());
        CAPTURE(engine.err());
        CHECK(has(engine.err(), state.path().string()));
        CHECK(lock_holder(engine.dir(), kEngineLockName) == engine.process().pid());
        CHECK(all_free(state.path()));
    }
}

TEST_CASE("State locks: an engine lock held elsewhere", "[state-folder][locks]") {
    const Files files;
    const std::expected<LockFile, LockFile::Failure> lock =
        LockFile::acquire(files.path(), kEngineLockName);
    REQUIRE(lock.has_value());
    const DirectEngine engine{files.path()};
    CHECK(engine.process().wait(kLaunchWait) == 1);
    CAPTURE(engine.err());
    CHECK(has(engine.err(), "another engine is running " + process_text(::getpid())));
    CHECK(all_free(engine.dir()));
}

TEST_CASE("State locks: options", "[state-folder][options]") {
    const Files files;
    const Folder dir;
    const std::string state = (files.path() / "state").string();
    const std::vector<std::string> twice{"--state-dir", state, "--state-dir", state};
    const std::vector<std::string> empty{"--state-dir", ""};

    for (const std::string_view program :
         {std::string_view{ks::processes::test::kKoolSpinPath},
          std::string_view{ks::processes::test::kEnginePath}, std::string_view{KS_PLAY_PATH}}) {
        CAPTURE(program);
        const Finished repeated = finished(files.path(), program, twice);
        CHECK(repeated.status == 2);
        CHECK(has(repeated.out, "--state-dir given twice"));
        const Finished nothing = finished(files.path(), program, empty);
        CHECK(nothing.status == 2);
        CHECK(has(nothing.out, "--state-dir needs a"));
        const Finished help = finished(files.path(), program, {"--help"});
        CHECK(help.status == 0);
        CHECK(has(help.out, "--state-dir <"));
    }

    // Once, it's taken: ks-play goes on to find that nothing is running.
    const Finished unreachable =
        finished(files.path(), KS_PLAY_PATH,
                 {"--state-dir", state, "--runtime-dir", dir.path().string(), "ping"});
    CHECK(unreachable.status == 3);
    CHECK(!fs::exists(state));
}
