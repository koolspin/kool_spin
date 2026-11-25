// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/supervisor-crash/spec.md: acquire() retries, so
// that a probe can't make it fail (requirement 1).

#include <ks/ipc/fd.h>
#include <ks/ipc/runtime_dir.h>

#include "crash_support.h"
#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <sys/file.h>

#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <thread>

namespace {

using namespace ks::supervisor_crash::test;
using ks::ipc::kEngineLockName;
using ks::ipc::LockFile;
using ks::ipc::test::TempDir;

/// Takes the lock on `path` as another process would: through a descriptor of
/// its own.
ks::ipc::Fd hold(const std::filesystem::path& path) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open() is variadic
    ks::ipc::Fd fd{::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600)};
    REQUIRE(fd.valid());
    REQUIRE(::flock(fd.get(), LOCK_EX | LOCK_NB) == 0);
    return fd;
}

} // namespace

TEST_CASE("Locks: acquire retries", "[crash][locks]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());

    SECTION("a lock held for 20 ms") {
        // A thread that a busy machine wakes late may hold the lock for
        // longer than acquire() tries: that run says nothing, so try again.
        bool said = false;
        for (int run = 0; run < 20 && !said; ++run) {
            ks::ipc::Fd held = hold(lock_path(dir.path(), kEngineLockName));
            const Clock::time_point began = Clock::now();
            Clock::time_point freed;
            std::thread holder{[&] {
                std::this_thread::sleep_for(Milliseconds{20});
                held.reset();
                freed = Clock::now();
            }};
            const std::expected<LockFile, LockFile::Failure> lock =
                LockFile::acquire(dir.path(), kEngineLockName);
            holder.join();
            said = freed - began < Milliseconds{30};
            if (said) {
                CHECK(lock.has_value());
            }
        }
        CHECK(said);
    }

    SECTION("a lock that stays held") {
        const Process holder = hold_lock(dir.path(), "stops");
        const Clock::time_point began = Clock::now();
        const std::expected<LockFile, LockFile::Failure> lock =
            LockFile::acquire(dir.path(), kEngineLockName);
        CHECK(Clock::now() - began < Milliseconds{500});
        REQUIRE_FALSE(lock.has_value());
        CHECK(lock.error().error == std::errc::resource_unavailable_try_again);
        CHECK(lock.error().holder == holder.pid());
    }
}

TEST_CASE("Locks: a probe never fails an acquire", "[crash][locks]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());
    std::atomic<bool> done{false};
    std::atomic<bool> probe_failed{false};
    std::thread prober{[&] {
        while (!done.load(std::memory_order_relaxed)) {
            if (!ks::ipc::probe_lock(dir.path(), kEngineLockName)) {
                probe_failed.store(true, std::memory_order_relaxed);
            }
        }
    }};
    int failed = 0;
    for (int i = 0; i < 200; ++i) {
        if (!LockFile::acquire(dir.path(), kEngineLockName)) {
            ++failed;
        }
    }
    done.store(true, std::memory_order_relaxed);
    prober.join();
    CHECK(failed == 0);
    CHECK_FALSE(probe_failed.load(std::memory_order_relaxed));
}
