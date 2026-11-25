// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/state-folder/spec.md: finding an engine from an
// earlier session through the state folder, or through the runtime directory
// (requirements 12 to 14).

#include <ks/ipc/runtime_dir.h>
#include <ks/ipc/state.h>

#include "state_support.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/types.h>

#include <algorithm>
#include <cmath>
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
#include <vector>

namespace {

using namespace ks::state_folder::test;
using ks::ipc::LockFile;
namespace fs = std::filesystem;
namespace restart = ks::engine_restart::test;

/// Checks that a launch in `dir` with `state` as its state folder finds
/// `orphan`, and leaves it playing. Returns what it said.
std::string check_found(const Files& files, const fs::path& dir, const fs::path& state,
                        const Playing& orphan, pid_t engine) {
    Launch launch{files.path(), launch_args(dir, with_state(state))};
    CHECK(launch.process().wait(kLaunchWait) == 1);
    CAPTURE(launch.err());
    CHECK(has(launch.err(), process_text(engine)));
    CHECK(has(launch.err(), "--stop-previous"));
    CHECK(!ended(engine));
    CHECK(keeps_beating(orphan.client.state));
    return launch.err();
}

/// Checks that a launch in `dir` with --stop-previous stops process `engine`
/// and starts a session, whose locks are in `dir`, and in `state` if
/// `state_lock`. The session has stopped when this returns.
void check_stopped(const Files& files, const fs::path& dir, const fs::path& state, pid_t engine,
                   bool state_lock = true) {
    Launch launch{files.path(), launch_args(dir, with_state(state, {"--stop-previous"}))};
    REQUIRE(new_session(dir, engine));
    CAPTURE(launch.err());
    CHECK(ended(engine));
    CHECK(has(launch.err(), "stopped the engine from an earlier session " + process_text(engine)));
    if (state_lock) {
        CHECK(held_by(state, kEngineLockName, engine_of(dir)));
    }
    CHECK(stop(dir));
    CHECK(launch.process().wait(kLaunchWait) == 0);
}

/// Checks that `orphan`, which has ended, faded out: its last block says so,
/// and its recording falls to silence without a step.
void check_faded(const Playing& orphan) {
    const auto status = restart::require(orphan.client.state.status());
    constexpr std::uint64_t kBoth = ks::ipc::kStatusMuted | ks::ipc::kStatusSilenced;
    CHECK((status.flags & kBoth) == kBoth);
    const std::uint64_t written = orphan.recording.reader().frames_written();
    REQUIRE(written > 20'000);
    const auto end = restart::require(orphan.recording.left(written - 20'000, 20'000));
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
}

} // namespace

TEST_CASE("State orphan: another runtime directory", "[state-folder][orphan]") {
    const Files files;
    const Folder state;
    const Folder other;
    const auto orphan = orphaned_with(files, state.path());
    const pid_t engine = orphan->session.engine();

    check_found(files, other.path(), state.path(), *orphan, engine);
    CHECK(all_free(other.path()));
    check_stopped(files, other.path(), state.path(), engine);
    check_faded(*orphan);
    CHECK(all_free(state.path()));
}

TEST_CASE("State orphan: a removed runtime directory", "[state-folder][orphan]") {
    const Files files;
    const Folder state;
    const auto orphan = orphaned_with(files, state.path());
    const pid_t engine = orphan->session.engine();
    const fs::path removed = orphan->session.dir();
    // Another name for the engine's lock file, so that the engine is killed at
    // the end even if the state folder doesn't know it.
    const Folder kept;
    fs::create_hard_link(lock_path(removed, kEngineLockName),
                         lock_path(kept.path(), kEngineLockName));
    REQUIRE(lock_holder(kept.path(), kEngineLockName) == engine);
    fs::remove_all(removed);
    REQUIRE(!fs::exists(removed));

    SECTION("another one") {
        const Folder other;
        check_found(files, other.path(), state.path(), *orphan, engine);
        check_stopped(files, other.path(), state.path(), engine);
    }
    SECTION("the same one, created again") {
        check_found(files, removed, state.path(), *orphan, engine);
        check_stopped(files, removed, state.path(), engine);
    }
    check_faded(*orphan);
}

TEST_CASE("State orphan: found through the runtime directory", "[state-folder][orphan]") {
    const Files files;
    const Folder place;
    const fs::path state = place.path() / "state";

    SECTION("the state folder's lock file deleted") {
        make_private(state);
        const auto orphan = orphaned_with(files, state);
        const pid_t engine = orphan->session.engine();
        REQUIRE(fs::remove(lock_path(state, kEngineLockName)));
        check_found(files, orphan->session.dir(), state, *orphan, engine);
        check_stopped(files, orphan->session.dir(), state, engine);
    }

    SECTION("a state folder that was never usable") {
        std::ofstream{state} << "contents";
        const auto orphan = orphaned_with(files, state);
        const pid_t engine = orphan->session.engine();
        check_found(files, orphan->session.dir(), state, *orphan, engine);
        check_stopped(files, orphan->session.dir(), state, engine, false);
    }

    SECTION("the state folder's lock file emptied") {
        make_private(state);
        const auto orphan = orphaned_with(files, state);
        const pid_t engine = orphan->session.engine();
        write_lock(state, "");
        REQUIRE(lock_holder(state, kEngineLockName) == -1);
        check_found(files, orphan->session.dir(), state, *orphan, engine);
        check_stopped(files, orphan->session.dir(), state, engine);
    }
}

TEST_CASE("State orphan: never found in a folder that's refused", "[state-folder][orphan]") {
    // Others can write the state folder now, so its process IDs can't be
    // trusted: the runtime directory is all that's left.
    const Files files;
    const Folder place;
    const Folder other;
    const fs::path state = place.path() / "state";
    make_private(state);
    const auto orphan = orphaned_with(files, state);
    const pid_t engine = orphan->session.engine();
    const Mode mode{state, 0777};

    check_found(files, orphan->session.dir(), state, *orphan, engine);
    Launch launch{files.path(), launch_args(other.path(), with_state(state, {"--stop-previous"}))};
    REQUIRE(new_session(other.path(), engine));
    CAPTURE(launch.err());
    CHECK(has(launch.err(), state.string()));
    CHECK(!has(launch.err(), process_text(engine)));
    CHECK(!ended(engine));
    CHECK(keeps_beating(orphan->client.state));
    CHECK(stop(other.path()));
    CHECK(launch.process().wait(kLaunchWait) == 0);
}

TEST_CASE("State orphan: a read-only state folder", "[state-folder][orphan]") {
    skip_as_root();
    const Files files;
    const Folder place;
    const Folder other;
    const fs::path state = place.path() / "state";
    make_private(state);
    const auto orphan = orphaned_with(files, state);
    const pid_t engine = orphan->session.engine();
    const Mode folder{state, 0500};
    const Mode supervisor_lock{lock_path(state, kSupervisorLockName), 0400};
    const Mode engine_lock{lock_path(state, kEngineLockName), 0400};

    const std::string said = check_found(files, other.path(), state, *orphan, engine);
    CHECK(has(said, state.string()));
    check_stopped(files, other.path(), state, engine, false);
    check_faded(*orphan);
}

TEST_CASE("State orphan: two of them", "[state-folder][orphan]") {
    const Files files;
    const Folder state;
    const Folder other;
    const auto orphan = orphaned_with(files, state.path());
    const pid_t engine = orphan->session.engine();
    Process helper = hold_lock(other.path(), "stops");

    Launch launch{files.path(),
                  launch_args(other.path(), with_state(state.path(), {"--stop-previous"}))};
    CHECK(launch.process().wait(kLaunchWait) == 1);
    CAPTURE(launch.err());
    CHECK(has(launch.err(), process_text(engine)));
    CHECK(has(launch.err(), process_text(helper.pid())));
    // It looked again itself: that isn't left to a new engine's own lock.
    CHECK(!has(launch.err(), "another engine is running"));
    CHECK(eventually([&] { return ended(engine); }));
    CHECK(helper.running());
    CHECK(lock_holder(other.path(), kEngineLockName) == helper.pid());
    CHECK(all_free(state.path()));
}

TEST_CASE("State orphan: ks-play's hint", "[state-folder][orphan]") {
    const Files files;
    const Folder state;
    const Folder other;
    const auto ask = [&](const std::vector<std::string>& options) {
        std::vector<std::string> args = options;
        args.emplace_back("status");
        return finished(files.path(), KS_PLAY_PATH, args);
    };
    const std::vector<std::string> both{"--runtime-dir", other.path().string(), "--state-dir",
                                        state.path().string()};
    constexpr std::string_view kHint = "an engine is running without kool_spin";

    const auto orphan = orphaned_with(files, state.path());
    const pid_t engine = orphan->session.engine();
    const auto in_state = files_in(state.path());

    SECTION("an orphan only the state folder knows") {
        const Finished said = ask(both);
        CHECK(said.status == 3);
        CAPTURE(said.out);
        CHECK(has(said.out, kHint));
        CHECK(has(said.out, process_text(engine)));
        // Without a runtime directory, too, which it doesn't create.
        const fs::path nowhere = files.path() / "nowhere";
        const Finished without =
            ask({"--runtime-dir", nowhere.string(), "--state-dir", state.path().string()});
        CHECK(without.status == 3);
        CHECK(has(without.out, kHint));
        CHECK(!fs::exists(nowhere));
        // Without the option, it doesn't look there.
        const Finished alone = ask({"--runtime-dir", other.path().string()});
        CHECK(alone.status == 3);
        CHECK(!has(alone.out, kHint));
    }

    SECTION("the process ID from the other folder") {
        write_lock(state.path(), "");
        const Finished said = ask({"--runtime-dir", orphan->session.dir().string(), "--state-dir",
                                   state.path().string()});
        CHECK(said.status == 3);
        CHECK(has(said.out, kHint));
        CHECK(has(said.out, process_text(engine)));
    }

    SECTION("the process ID from the state folder, when the other has none") {
        write_lock(orphan->session.dir(), "");
        const Finished said = ask({"--runtime-dir", orphan->session.dir().string(), "--state-dir",
                                   state.path().string()});
        CHECK(said.status == 3);
        CHECK(has(said.out, kHint));
        CHECK(has(said.out, process_text(engine)));
    }

    SECTION("an orphan only the runtime directory knows") {
        const Files empty;
        const Finished said = ask({"--runtime-dir", orphan->session.dir().string(), "--state-dir",
                                   empty.path().string()});
        CHECK(said.status == 3);
        CHECK(has(said.out, kHint));
        CHECK(has(said.out, process_text(engine)));
        CHECK(files_in(empty.path()).empty());
    }

    SECTION("a supervisor in the state folder") {
        const std::expected<LockFile, LockFile::Failure> lock =
            LockFile::acquire(state.path(), kSupervisorLockName);
        REQUIRE(lock.has_value());
        const Finished said = ask(both);
        CHECK(said.status == 3);
        CHECK(!has(said.out, kHint));
    }

    SECTION("a supervisor in the runtime directory") {
        {
            const std::expected<LockFile, LockFile::Failure> lock =
                LockFile::acquire(other.path(), kSupervisorLockName);
            REQUIRE(lock.has_value());
            const Finished said = ask(both);
            CHECK(said.status == 3);
            CHECK(!has(said.out, kHint));
        }
        REQUIRE(fs::remove(lock_path(other.path(), kSupervisorLockName)));
    }

    SECTION("a state folder that others can write") {
        const Mode mode{state.path(), 0777};
        const Finished said = ask(both);
        CHECK(said.status == 3);
        CHECK(!has(said.out, kHint));
    }

    CHECK(files_in(state.path()) == in_state);
    CHECK(files_in(other.path()).empty());
    CHECK(!ended(engine));
}
