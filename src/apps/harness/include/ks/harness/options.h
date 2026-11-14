// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// ks-harness's command line (specs/harness/spec.md requirements 2 and 3).

#include <ks/engine/miniaudio_backend.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace ks::harness {

// Ranges and defaults of the options (requirement 2).
inline constexpr std::uint32_t kMinRate = 8'000;
inline constexpr std::uint32_t kMaxRate = 384'000;
inline constexpr std::uint32_t kMinChannels = 2;
inline constexpr std::uint32_t kMaxChannels = 8;
inline constexpr std::uint32_t kMinPeriod = 1;
inline constexpr std::uint32_t kMaxPeriod = 4'096;
inline constexpr std::uint32_t kDefaultFileRate = 48'000;
inline constexpr std::uint32_t kDefaultFileChannels = 2;
inline constexpr std::uint32_t kDefaultFileBlock = 256;

/// The exit codes (requirement 3).
inline constexpr int kExitOk = 0;
inline constexpr int kExitRuntimeError = 1;
inline constexpr int kExitUsageError = 2;
inline constexpr int kExitProblem = 3;

/// ks-harness's command line, parsed. Options not given are empty.
struct Options {
    /// Empty only with --list-devices or --help.
    std::optional<std::filesystem::path> script;
    std::optional<std::filesystem::path> out;
    std::optional<engine::MiniaudioBackends> backend;
    std::optional<std::size_t> device;
    /// 0 is allowed only in device mode.
    std::optional<std::uint32_t> rate;
    std::optional<std::uint32_t> channels;
    std::optional<std::uint32_t> period;
    bool list_devices = false;
    bool check = false;
    bool help = false;
};

/// The usage text that --help and usage errors print. Its first line is
/// "Usage: ks-harness [options] <script>".
[[nodiscard]] std::string usage();

/// Parses ks-harness's arguments, not including argv[0]. Parsing stops at
/// --help, which sets `help`. On a usage error, returns a one-line message,
/// for example "ks-harness: --rate must be a whole number from 8000 to
/// 384000". A device number is checked against the device list by run(),
/// not here.
[[nodiscard]] std::expected<Options, std::string>
parse_options(std::span<const std::string_view> args);

/// Does what `options` asks: --help, --list-devices, --check, or reading the
/// script, preparing its tracks, and playing it into a file or through a
/// device. Writes normal output to `out`, and errors to `err`: a script
/// error as "<script>:<line>: <message>". Returns the exit code
/// (requirement 3). `stop` is passed on to the player.
[[nodiscard]] int run(const Options& options, std::ostream& out, std::ostream& err,
                      const std::atomic<bool>& stop);

} // namespace ks::harness
