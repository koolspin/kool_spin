// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/health-checks/spec.md, step 10a, through the
// programs: kool_spin's engine on miniaudio's null backend runs its health
// passes, logs what fails, and `ks-play health` prints the health block.
// Limits are set by ks_rlimit_exec before it executes kool_spin, so the
// engine inherits them. The power checks read a fake /sys through the
// test-only `-- --sysfs-root` (Linux).
//
// Tests that need more than an unprivileged user gets (RLIMIT_RTPRIO of 70,
// an unlimited RLIMIT_MEMLOCK) skip when the hard limit doesn't allow it;
// tests that need a limit to hold skip as root, which ignores it.

#include <ks/ipc/state.h>

#include "health_support.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace ks::health_checks::test;

/// A track's files' folder.
struct Files {
    ks::ipc::test::TempDir dir;
    [[nodiscard]] std::filesystem::path ten_seconds() const {
        return restart::write_sine(dir.path(), "ten.wav", 10);
    }
};

/// Waits until the engine's log has `count` lines containing `text`, and
/// returns how many it has.
std::size_t log_lines(const HealthSession& session, std::string_view text, std::size_t count,
                      Milliseconds timeout = Milliseconds{3'000} * kSlack) {
    std::size_t found = 0;
    static_cast<void>(eventually(
        [&] {
            found = count_lines(session.engine_log(), text);
            return found >= count;
        },
        timeout));
    return found;
}

/// Options with only `engine_options` set.
HealthOptions engine_options(std::vector<std::string> options) {
    HealthOptions health;
    health.engine_options = std::move(options);
    return health;
}

/// Options with only these limits set.
HealthOptions limits(std::optional<Limit> rtprio, std::optional<Limit> memlock) {
    HealthOptions health;
    health.limits.rtprio = std::move(rtprio);
    health.limits.memlock = std::move(memlock);
    return health;
}

/// The policy, priority pairs of `pid`'s threads that are SCHED_FIFO (1).
bool has_fifo_thread(pid_t pid, int priority) {
    const std::vector<std::pair<int, int>> threads = thread_scheduling(pid);
    return std::ranges::any_of(threads, [&](const std::pair<int, int>& thread) {
        return thread.first == 1 && thread.second == priority;
    });
}

} // namespace

TEST_CASE("Health: a change is logged once", "[health][process]") {
#ifndef __linux__
    SKIP("--sysfs-root is Linux only");
#else
    const FakeSys sys;
    sys.platform_profile("balanced");
    sys.supply("AC", "Mains", "online", "1");
    const HealthSession session{engine_options({"--sysfs-root", sys.path().string()})};

    const HealthRun normal = health_until(
        session.dir(), [](const HealthRun& run) { return run.get("power_profile") == "normal"; });
    CAPTURE(normal.finished.out, normal.finished.err, session.engine_log());
    REQUIRE(normal.get("power_profile") == "normal");
    CHECK(normal.get("power_source") == "mains");
    CHECK_FALSE(normal.fails("power-profile"));

    const std::string warning = "health: the CPU is in a power-saving profile "
                                "(firmware/acpi/platform_profile is low-power)";
    sys.platform_profile("low-power");
    const HealthRun saving = health_until(
        session.dir(), [](const HealthRun& run) { return run.get("power_profile") == "saving"; },
        Milliseconds{2'000} * kSlack);
    CHECK(saving.get("power_profile") == "saving");
    CHECK(saving.fails("power-profile"));
    CHECK(log_lines(session, warning, 1) == 1);

    // Five more passes: still one warning.
    const std::expected<HealthView, std::string> view = HealthView::open(session.dir());
    REQUIRE(view.has_value());
    const std::uint64_t passes = view->passes();
    REQUIRE(eventually([&] { return view->passes() >= passes + 5; }, Milliseconds{8'000} * kSlack));
    CHECK(count_lines(session.engine_log(), warning) == 1);

    // Back to balanced: one info line.
    sys.platform_profile("balanced");
    const std::string fine = "health: power profile is fine again";
    CHECK(log_lines(session, fine, 1) == 1);
    CHECK(health_until(session.dir(), [](const HealthRun& run) {
              return run.get("power_profile") == "normal";
          }).get("power_profile") == "normal");
    std::this_thread::sleep_for(Milliseconds{2'500});
    CHECK(count_lines(session.engine_log(), fine) == 1);
    CHECK(count_lines(session.engine_log(), warning) == 1);
#endif
}

TEST_CASE("Health: real-time priority refused", "[health][process]") {
#ifndef __linux__
    SKIP("macOS needs no privilege for real-time priority");
#else
    if (is_root()) {
        SKIP("root ignores RLIMIT_RTPRIO");
    }
    const Files files;
    const HealthSession session{limits(Limit{.soft = "0", .hard = "0"}, std::nullopt)};
    load_and_play(session.dir(), files.ten_seconds());
    CHECK(deck0_plays(session.dir()));

    const HealthRun run = health_until(
        session.dir(), [](const HealthRun& health) { return health.get("realtime") == "0"; });
    CAPTURE(run.finished.out, run.finished.err, session.engine_log());
    CHECK(run.get("realtime") == "0");
    CHECK(run.get("policy") == "other");
    CHECK(run.get("rtprio_limit") == "0");
    CHECK(run.fails("realtime"));
    CHECK(log_lines(session,
                    "health: the audio thread isn't real-time (other); RLIMIT_RTPRIO is 0, and 70 "
                    "is needed",
                    1) == 1);
    CHECK(deck0_plays(session.dir()));
#endif
}

TEST_CASE("Health: real-time priority granted", "[health][process]") {
#ifdef __linux__
    if (!is_root() && hard_limit(RLIMIT_RTPRIO) < 70) {
        SKIP("the hard RLIMIT_RTPRIO is below 70");
    }
    const HealthSession session{limits(Limit{.soft = "70", .hard = "keep"}, std::nullopt)};
#else
    const HealthSession session;
#endif
    const HealthRun run = health_until(
        session.dir(), [](const HealthRun& health) { return health.get("realtime") == "1"; });
    CAPTURE(run.finished.out, run.finished.err, session.engine_log());
    CHECK(run.get("realtime") == "1");
    CHECK_FALSE(run.fails("realtime"));
#ifdef __linux__
    CHECK(run.get("policy") == "fifo");
    CHECK(run.get("priority") == "70");
    CHECK(has_fifo_thread(session.engine(), 70));
#else
    CHECK(run.get("policy") == "time-constraint");
#endif
    CHECK(count_lines(session.engine_log(), "isn't real-time") == 0);
}

TEST_CASE("Health: memory locking refused", "[health][process]") {
#ifndef __linux__
    SKIP("macOS has no mlockall(), and its RLIMIT_MEMLOCK isn't checked here");
#else
    if (is_root()) {
        SKIP("root ignores RLIMIT_MEMLOCK");
    }
    const Files files;
    const HealthSession session{limits(std::nullopt, Limit{.soft = "65536", .hard = "65536"})};
    load_and_play(session.dir(), files.ten_seconds());

    const HealthRun run = health_until(session.dir(), [](const HealthRun& health) {
        return health.number("unlocked_bytes") >= kTenSecondBytes;
    });
    CAPTURE(run.finished.out, run.finished.err, session.engine_log());
    CHECK(run.get("process_lock") == "failed");
    CHECK(run.get("memlock_limit") == "65536");
    CHECK(run.number("unlocked_bytes") >= kTenSecondBytes);
    CHECK(run.get("memory") == "0");
    CHECK(run.fails("memory"));
    CHECK(log_lines(session, "health: memory isn't locked (", 1) == 1);
    CHECK(count_lines(session.engine_log(), "RLIMIT_MEMLOCK is") == 1);
    // The track plays on.
    CHECK(deck0_plays(session.dir()));
#endif
}

TEST_CASE("Health: memory locked", "[health][process]") {
#ifdef __linux__
    if (!is_root() && hard_limit(RLIMIT_MEMLOCK) != RLIM_INFINITY) {
        SKIP("the hard RLIMIT_MEMLOCK isn't unlimited");
    }
    const HealthSession session{limits(std::nullopt, Limit{.soft = "unlimited", .hard = "keep"})};
#else
    const HealthSession session;
#endif
    const Files files;
    load_and_play(session.dir(), files.ten_seconds());

    const HealthRun run = health_until(session.dir(), [](const HealthRun& health) {
        return health.number("locked_bytes") >= kTenSecondBytes;
    });
    CAPTURE(run.finished.out, run.finished.err, session.engine_log());
    CHECK(run.number("locked_bytes") >= kTenSecondBytes);
    CHECK(run.get("unlocked_bytes") == "0");
    CHECK(run.get("memory") == "1");
    CHECK_FALSE(run.fails("memory"));
#ifdef __linux__
    CHECK(run.get("process_lock") == "locked");
    CHECK(run.get("memlock_limit") == "unlimited");
    const pid_t engine = session.engine();
    REQUIRE(engine > 0);
    const std::optional<std::uint64_t> locked = proc_status_bytes(engine, "VmLck");
    const std::optional<std::uint64_t> resident = proc_status_bytes(engine, "VmRSS");
    CAPTURE(locked, resident);
    CHECK(restart::require(locked) >= kTenSecondBytes);
    // The track segments' unused space, sized at the 2 GiB cap, isn't
    // committed.
    CHECK(restart::require(resident) < std::uint64_t{256} * 1024 * 1024);
#else
    CHECK(run.get("process_lock") == "none");
#endif
    CHECK(count_lines(session.engine_log(), "memory isn't locked") == 0);
}

TEST_CASE("Play: health", "[health][process]") {
    SECTION("every field, in order") {
        HealthOptions options;
#ifdef __linux__
        // Nothing fails only where an unprivileged user may have real-time
        // priority and lock memory; elsewhere, the power checks still pass.
        const bool privileged = is_root() || (hard_limit(RLIMIT_RTPRIO) >= 70 &&
                                              hard_limit(RLIMIT_MEMLOCK) == RLIM_INFINITY);
        const FakeSys sys;
        sys.platform_profile("balanced");
        sys.supply("AC", "Mains", "online", "1");
        options.engine_options = {"--sysfs-root", sys.path().string()};
        if (privileged) {
            options.limits.rtprio = Limit{.soft = "70", .hard = "keep"};
            options.limits.memlock = Limit{.soft = "unlimited", .hard = "keep"};
        }
#else
        const bool privileged = true;
#endif
        const HealthSession session{options};
        const HealthRun run = health_until(session.dir(), [](const HealthRun&) { return true; });
        CAPTURE(run.finished.out, run.finished.err);
        CHECK(run.finished.status == 0);
        CHECK(run.finished.err.empty());
        const play::StatusLine line = restart::require(run.line);
        CHECK(line.label == "health");
        std::vector<std::string> names;
        names.reserve(line.fields.size());
        for (const auto& field : line.fields) {
            names.push_back(field.first);
        }
        CHECK(names == health_fields());

        // Each value is one requirement 19 allows.
        const auto one_of = [&](std::string_view name, const std::vector<std::string>& allowed) {
            const std::optional<std::string> value = run.get(name);
            CAPTURE(name, value);
            CHECK((value && std::ranges::find(allowed, *value) != allowed.end()));
        };
        one_of("realtime", {"0", "1", "unknown"});
        one_of("policy", {"unknown", "other", "fifo", "rr", "time-constraint"});
        one_of("memory", {"0", "1", "unknown"});
        one_of("process_lock", {"none", "locked", "failed"});
        one_of("power_profile", {"unknown", "normal", "saving"});
        one_of("power_source", {"unknown", "mains", "battery"});
        one_of("awake", {"unknown", "blocked", "not-blocked"});
        CHECK_FALSE(run.fails("power-profile"));
        CHECK_FALSE(run.fails("power-source"));
        if (privileged) {
            CHECK(run.get("failing") == "none");
        }
    }

    SECTION("no health pass in 2 s") {
        // The control thread stops answering at once after Ready, before its
        // first pass; pings are rare enough that the supervisor leaves it.
        HealthOptions options = engine_options({"--hang-control-after", "0"});
        options.paced = true;
        options.supervisor = {"--ping-interval", "60000"};
        const HealthSession session{options};
        const auto started = Clock::now();
        const HealthRun run = run_health(session.dir());
        const auto took = Clock::now() - started;
        CAPTURE(run.finished.out, run.finished.err);
        CHECK(run.finished.status == 1);
        CHECK(run.finished.out.empty());
        CHECK(run.finished.err.find("ks-play: no health check yet") != std::string::npos);
        CHECK(took >= Milliseconds{2'000});
    }

    SECTION("--help lists it") {
        const ks::ipc::test::TempDir scratch;
        const play::Finished help = play::run_play_raw(scratch.path(), {"--help"});
        CHECK(help.status == 0);
        CHECK(help.out.find("  health") != std::string::npos);
    }
}

TEST_CASE("Health: after an engine restart", "[health][process]") {
    HealthOptions options;
#ifdef __linux__
    // A check that fails in every engine, so each logs it.
    const FakeSys sys;
    sys.platform_profile("low-power");
    options.engine_options = {"--sysfs-root", sys.path().string()};
    const std::string warning = "health: the CPU is in a power-saving profile";
#endif
    const HealthSession session{options};
    const std::expected<HealthView, std::string> view = HealthView::open(session.dir());
    REQUIRE(view.has_value());
    REQUIRE(eventually([&] { return view->passes() >= 4; }, Milliseconds{8'000} * kSlack));
#ifdef __linux__
    REQUIRE(log_lines(session, warning, 1) == 1);
#endif

    const pid_t old = session.engine();
    REQUIRE(old > 0);
    const std::uint64_t before = view->passes();
    REQUIRE(::kill(old, SIGKILL) == 0);
    REQUIRE(eventually(
        [&] {
            const pid_t now = session.engine();
            return now > 0 && now != old;
        },
        restart::kRestartWait));

    // The new engine counts its own passes, from 1.
    std::uint64_t after = 0;
    CHECK(eventually(
        [&] {
            after = view->passes();
            return after >= 1 && after < before;
        },
        Milliseconds{5'000} * kSlack));
    CAPTURE(before, after);
#ifdef __linux__
    // And logs its checks afresh.
    CHECK(log_lines(session, warning, 2) == 2);
#endif
}
