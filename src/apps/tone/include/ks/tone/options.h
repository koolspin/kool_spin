// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ks/tone/tone_generator.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ks::tone {

// Ranges and defaults of ks-tone's options. --help prints these.
inline constexpr double kMinFrequencyHz = 20.0;
inline constexpr double kMaxFrequencyHz = 20000.0;
inline constexpr double kDefaultFrequencyHz = 440.0;
inline constexpr double kMinLevelDbfs = -60.0;
inline constexpr double kMaxLevelDbfs = -6.0; ///< A safety cap, not a technical limit.
inline constexpr double kDefaultLevelDbfs = -20.0;
inline constexpr double kMinDurationSeconds = 0.1;
inline constexpr double kMaxDurationSeconds = 60.0;
inline constexpr double kDefaultDurationSeconds = 2.0;

/// ks-tone's command line, parsed.
struct Options {
    std::string backend; ///< Empty: the first backend that works.
    ToneSettings settings{.frequency_hz = kDefaultFrequencyHz, .level_dbfs = kDefaultLevelDbfs};
    std::chrono::duration<double> duration{kDefaultDurationSeconds};
    std::optional<std::uint32_t> device; ///< Index from --list-devices; empty: the default.
    bool list_devices = false;
    bool help = false;
};

/// Parses ks-tone's arguments, not including argv[0].
///
/// Parsing stops at -h or --help, which sets `help`. On a usage error, returns
/// the complete one-line message to print, for example
/// "ks-tone: --level must be a number from -60 to -6".
[[nodiscard]] std::expected<Options, std::string>
parse_options(std::span<const std::string_view> args);

} // namespace ks::tone
