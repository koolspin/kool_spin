// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/supervisor-crash/spec.md: finding an engine that
// outlived its supervisor, stopping it, and never signaling anything else
// (requirements 2 to 4).

#include <ks/supervisor/orphan.h>

#include <ks/ipc/runtime_dir.h>
#include <ks/supervisor/run.h>

#include "crash_support.h"
#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace {

using namespace ks::supervisor_crash::test;
using ks::ipc::test::TempDir;
using ks::supervisor::find_orphan;
using ks::supervisor::kEngineStopTimeout;
using ks::supervisor::kOrphanSettleLimit;
using ks::supervisor::Orphan;
using ks::supervisor::stop_orphan;
using ks::supervisor::Stopped;

/// What find_orphan() found, which must not be an error.
std::optional<Orphan> found(const TempDir& dir, const ks::supervisor::Wait& wait = sleep_for) {
    const std::expected<std::optional<Orphan>, std::error_code> result =
        find_orphan(dir.path(), wait);
    REQUIRE(result.has_value());
    return result.value_or(std::nullopt);
}

/// True if `orphan` is there, with exactly `pid`.
bool is(const std::optional<Orphan>& orphan, std::optional<pid_t> pid) {
    return orphan && orphan->pid == pid;
}

} // namespace

TEST_CASE("Orphan: finding it", "[crash][orphan]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());

    SECTION("no file") {
        CHECK_FALSE(found(dir).has_value());
    }

    SECTION("a free lock") {
        write_lock(dir.path(), ::getppid());
        CHECK_FALSE(found(dir).has_value());
    }

    SECTION("a held lock") {
        const Process holder = hold_lock(dir.path(), "stops");
        CHECK(is(found(dir), holder.pid()));
    }

    SECTION("a file without a valid ID") {
        const Process holder = hold_lock(dir.path(), "stops");
        for (const std::string_view contents :
             {"", "\n", "0\n", "-5\n", "12x\n", "99999999999999\n"}) {
            CAPTURE(contents);
            write_lock(dir.path(), contents);
            CHECK(is(found(dir, [](Milliseconds) {}), std::nullopt));
        }
    }

    SECTION("the caller's own ID") {
        const Process holder = hold_lock(dir.path(), "stops");
        write_lock(dir.path(), ::getpid());
        CHECK(is(found(dir, [](Milliseconds) {}), std::nullopt));
    }

    SECTION("an ID that changes once") {
        const Process holder = hold_lock(dir.path(), "stops");
        const Process before = bystander("stops");
        write_lock(dir.path(), before.pid());
        int waits = 0;
        const auto settle = [&](Milliseconds) {
            if (++waits == 1) {
                write_lock(dir.path(), holder.pid());
            }
        };
        CHECK(is(found(dir, settle), holder.pid()));
        CHECK(waits == 2);
    }

    SECTION("an ID that changes at every probe") {
        const Process holder = hold_lock(dir.path(), "stops");
        const Process other = bystander("stops");
        bool mine = true;
        Milliseconds waited{0};
        const auto change = [&](Milliseconds time) {
            waited += time;
            mine = !mine;
            write_lock(dir.path(), mine ? holder.pid() : other.pid());
        };
        CHECK(is(found(dir, change), std::nullopt));
        CHECK(waited == kOrphanSettleLimit);
    }

    SECTION("a lock that comes free meanwhile") {
        Process holder = hold_lock(dir.path(), "stops");
        const auto free = [&](Milliseconds) {
            holder.signal(SIGKILL);
            static_cast<void>(holder.wait());
        };
        CHECK_FALSE(found(dir, free).has_value());
    }

    SECTION("a lock file that can't be read") {
        std::filesystem::create_symlink("elsewhere", lock_path(dir.path(), "engine"));
        CHECK_FALSE(find_orphan(dir.path(), sleep_for).has_value());
    }
}

TEST_CASE("Orphan: stopping it", "[crash][orphan]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());

    SECTION("one that stops") {
        Process holder = hold_lock(dir.path(), "stops");
        const std::expected<Stopped, std::string> stopped =
            stop_orphan(dir.path(), holder.pid(), sleep_for);
        CHECK(stopped == Stopped::Faded);
        CHECK(holder.wait() == 0);
    }

    SECTION("one that doesn't") {
        Process holder = hold_lock(dir.path(), "ignores");
        const Clock::time_point began = Clock::now();
        const std::expected<Stopped, std::string> stopped =
            stop_orphan(dir.path(), holder.pid(), sleep_for);
        CHECK(stopped == Stopped::Killed);
        CHECK(Clock::now() - began >= kEngineStopTimeout);
        CHECK(holder.wait() == 128 + SIGKILL);
    }

    SECTION("one that's frozen") {
        Process holder = hold_lock(dir.path(), "stops");
        holder.signal(SIGSTOP);
        const std::expected<Stopped, std::string> stopped =
            stop_orphan(dir.path(), holder.pid(), sleep_for);
        CHECK(stopped == Stopped::Killed);
        CHECK(holder.wait() == 128 + SIGKILL);
    }
}

TEST_CASE("Orphan: never signals another process", "[crash][orphan]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());

    SECTION("the lock names another process from the start") {
        Process holder = hold_lock(dir.path(), "stops");
        Process other = bystander("stops");
        CHECK_FALSE(stop_orphan(dir.path(), other.pid(), sleep_for).has_value());
        sleep_for(Milliseconds{200});
        CHECK(other.running());
        CHECK(holder.running());
    }

    SECTION("the lock names no process") {
        Process holder = hold_lock(dir.path(), "stops");
        write_lock(dir.path(), "");
        CHECK_FALSE(stop_orphan(dir.path(), holder.pid(), sleep_for).has_value());
        sleep_for(Milliseconds{200});
        CHECK(holder.running());
    }

    SECTION("the lock names another process before SIGKILL") {
        Process holder = hold_lock(dir.path(), "stops");
        Process first = bystander("ignores");
        Process second = bystander("stops");
        write_lock(dir.path(), first.pid());
        bool changed = false;
        const auto change = [&](Milliseconds time) {
            if (!changed) {
                changed = true;
                write_lock(dir.path(), second.pid());
            }
            sleep_for(time);
        };
        CHECK_FALSE(stop_orphan(dir.path(), first.pid(), change).has_value());
        CHECK(changed);
        sleep_for(Milliseconds{200});
        CHECK(first.running());
        CHECK(second.running());
        CHECK(holder.running());
    }

    SECTION("the lock is free before the first signal") {
        Process other = bystander("stops");
        write_lock(dir.path(), other.pid());
        CHECK(stop_orphan(dir.path(), other.pid(), sleep_for) == Stopped::Gone);
        sleep_for(Milliseconds{200});
        CHECK(other.running());
    }
}
