// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Helpers for the supervisor-crash acceptance tests (specs/supervisor-crash/
// spec.md) that don't need a running kool_spin: processes that hold the engine
// lock, and the lock file itself.

#include <ks/ipc/runtime_dir.h>

#include "processes_support.h"
#include "segments_support.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/types.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef KS_LOCK_HOLDER_PATH
#error "KS_LOCK_HOLDER_PATH must name the ks_lock_holder executable"
#endif

namespace ks::supervisor_crash::test {

using processes::test::Clock;
using processes::test::eventually;
using processes::test::Milliseconds;
using processes::test::Process;

/// The lock file `name` in `dir`.
inline std::filesystem::path lock_path(const std::filesystem::path& dir, std::string_view name) {
    return dir / std::format("{}.lock", name);
}

/// Replaces the contents of `dir`'s engine lock file.
inline void write_lock(const std::filesystem::path& dir, std::string_view contents) {
    std::ofstream out{lock_path(dir, ipc::kEngineLockName), std::ios::trunc | std::ios::binary};
    out << contents;
    out.close();
    REQUIRE(out.good());
}

inline void write_lock(const std::filesystem::path& dir, pid_t pid) {
    write_lock(dir, std::format("{}\n", pid));
}

/// A process that holds `dir`'s engine lock, in `mode` (lock_holder.cpp), once
/// its ID is in the file.
inline Process hold_lock(const std::filesystem::path& dir, std::string_view mode) {
    Process process =
        processes::test::start(KS_LOCK_HOLDER_PATH, {std::string{mode}, dir.string()});
    REQUIRE(process.pid() > 0);
    REQUIRE(eventually(
        [&] { return processes::test::lock_holder(dir, ipc::kEngineLockName) == process.pid(); }));
    return process;
}

/// A process that holds nothing, and ignores SIGTERM if `mode` is `ignores`.
inline Process bystander(std::string_view mode) {
    Process process = processes::test::start(KS_LOCK_HOLDER_PATH, {std::string{mode}});
    REQUIRE(process.pid() > 0);
    // It has set SIGTERM up by then.
    std::this_thread::sleep_for(Milliseconds{200});
    REQUIRE(process.running());
    return process;
}

/// A Wait that waits.
inline void sleep_for(Milliseconds time) {
    std::this_thread::sleep_for(time);
}

} // namespace ks::supervisor_crash::test
