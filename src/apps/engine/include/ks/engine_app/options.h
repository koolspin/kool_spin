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
#include <vector>

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

/// The most `--track` options one command line takes
/// (specs/engine-restart/spec.md requirement 4).
inline constexpr std::size_t kMaxTrackOptions = 16;

/// A `--track <id>:<fd>` (specs/engine-restart/spec.md requirement 5).
struct TrackOption {
    /// The track's ID, never 0.
    std::uint64_t id = 0;
    int fd = -1;

    friend bool operator==(const TrackOption&, const TrackOption&) = default;
};

/// ks-engine's command line, parsed. Options not given are empty.
struct Options {
    /// Its end of the socket pair to the supervisor.
    std::optional<int> supervisor_fd;
    /// The engine-state segment.
    std::optional<int> state_fd;
    /// The log segment, if the supervisor gives one (specs/logs/spec.md
    /// requirement 16).
    std::optional<int> log_fd;
    /// The recording segment, if the supervisor gives one (`--record-fd`,
    /// specs/engine-restart/spec.md requirement 14).
    std::optional<int> record_fd;
    /// The tracks a restarted engine is given, in the order of the command
    /// line (`--track`, requirement 5): at most kMaxTrackOptions, with no
    /// repeated ID or descriptor.
    std::vector<TrackOption> tracks;
    /// `--clean`: start as at launch, ignoring the resume state and --track
    /// (requirement 11).
    bool clean = false;
    /// `--backend paced`: render on a paced backend instead of a device
    /// (requirement 16). `backend` is then empty.
    bool paced = false;
    /// `--start-delay <ms>`: wait this long before building the engine.
    /// Only `--backend paced` accepts it (requirement "test-only options").
    std::optional<std::uint32_t> start_delay_ms;
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
