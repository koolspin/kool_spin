// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// kool_spin's command line and exit statuses (specs/processes/spec.md
/// requirements 13 and 14; specs/track-loading/spec.md requirement 9).

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ks::supervisor {

/// The exit statuses (requirement 14).
inline constexpr int kExitOk = 0;
/// It couldn't start.
inline constexpr int kExitStartFailed = 1;
inline constexpr int kExitUsageError = 2;
/// The engine ended unexpectedly, or didn't stop when asked.
inline constexpr int kExitEngineFailed = 3;

/// The smallest and largest track memory budgets --track-memory takes, in MiB.
inline constexpr std::uint64_t kMinTrackMemoryMib = 1;
inline constexpr std::uint64_t kMaxTrackMemoryMib = 1'048'576;

/// kool_spin's command line, parsed. Options not given are empty.
struct Options {
    std::optional<std::filesystem::path> runtime_dir;
    /// The folder that holds the session folders (specs/logs/spec.md
    /// requirement 9).
    std::optional<std::filesystem::path> log_dir;
    /// The ks-engine executable.
    std::optional<std::filesystem::path> engine;
    /// The ks-worker executable.
    std::optional<std::filesystem::path> worker;
    /// The track memory budget, in MiB: kMinTrackMemoryMib to
    /// kMaxTrackMemoryMib.
    std::optional<std::uint64_t> track_memory_mib;
    /// `--recording`: create a recording segment and pass it to every engine
    /// (specs/engine-restart/spec.md requirement 14).
    bool recording = false;
    /// `--crash-window <ms>` and `--ready-timeout <ms>`: replace
    /// kCrashLoopWindow and kEngineReadyTimeout, for tests. 1 to 3,600,000.
    std::optional<std::uint64_t> crash_window_ms;
    std::optional<std::uint64_t> ready_timeout_ms;
    /// `--hang-timeout <ms>` and `--ping-interval <ms>`: replace
    /// kHangTimeout and kEnginePingInterval, for tests
    /// (specs/hang-detection/spec.md requirement 14). 1 to 3,600,000.
    std::optional<std::uint64_t> hang_timeout_ms;
    std::optional<std::uint64_t> ping_interval_ms;
    /// Everything after "--", for ks-engine, unchecked.
    std::vector<std::string> engine_args;
    bool help = false;
};

/// The usage text that --help and usage errors print. Its first line is
/// "Usage: kool_spin [options] [-- engine options]".
[[nodiscard]] std::string usage();

/// Parses kool_spin's arguments, not including argv[0]. Parsing stops at
/// --help, which sets `help`, and at "--", after which every argument goes to
/// `engine_args`. On a usage error, returns a one-line message starting
/// "kool_spin: ".
[[nodiscard]] std::expected<Options, std::string>
parse_options(std::span<const std::string_view> args);

} // namespace ks::supervisor
