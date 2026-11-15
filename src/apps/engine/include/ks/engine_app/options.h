// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// ks-engine's command line and exit statuses (specs/processes/spec.md
/// requirement 8).

#include <ks/engine/miniaudio_backend.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ks::engine_app {

// The output options' ranges: ks-harness's (specs/harness/spec.md
// requirement 2).
inline constexpr std::uint32_t kMinRate = 8'000;
inline constexpr std::uint32_t kMaxRate = 384'000;
inline constexpr std::uint32_t kMinChannels = 2;
inline constexpr std::uint32_t kMaxChannels = 8;
inline constexpr std::uint32_t kMinPeriod = 1;
inline constexpr std::uint32_t kMaxPeriod = 4'096;

/// The exit statuses (requirements 8, 9, and 11).
inline constexpr int kExitOk = 0;
/// It couldn't start.
inline constexpr int kExitStartFailed = 1;
inline constexpr int kExitUsageError = 2;

/// ks-engine's command line, parsed. Options not given are empty.
struct Options {
    /// Its end of the socket pair to the supervisor.
    std::optional<int> supervisor_fd;
    /// The engine-state segment.
    std::optional<int> state_fd;
    std::optional<std::filesystem::path> runtime_dir;
    std::optional<engine::MiniaudioBackends> backend;
    /// A device number, as `ks-harness --list-devices` numbers them. Checked
    /// against the device list by run(), not here.
    std::optional<std::size_t> device;
    std::optional<std::uint32_t> rate;
    std::optional<std::uint32_t> channels;
    std::optional<std::uint32_t> period;
    bool help = false;
};

/// The usage text that --help and usage errors print. Its first line is
/// "Usage: ks-engine --supervisor-fd <n> --state-fd <n> --runtime-dir <path>
/// [output options]".
[[nodiscard]] std::string usage();

/// Parses ks-engine's arguments, not including argv[0]. Parsing stops at
/// --help, which sets `help`. A missing required option, an unknown or
/// repeated option, or a value out of range is a usage error: returns a
/// one-line message starting "ks-engine: ".
[[nodiscard]] std::expected<Options, std::string>
parse_options(std::span<const std::string_view> args);

} // namespace ks::engine_app
