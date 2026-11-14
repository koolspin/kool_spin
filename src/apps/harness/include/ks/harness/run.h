// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Playing a script, into a file or through a device, and the report
/// (specs/harness/spec.md requirements 17 to 20 and 24 to 29).

#include <ks/engine/miniaudio_backend.h>
#include <ks/harness/script.h>
#include <ks/harness/tracks.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>

namespace ks::harness {

/// How a run went (requirement 28).
struct Report {
    /// Frames rendered divided by the sample rate.
    double played_seconds = 0.0;
    /// The stop flag ended it before the script's end.
    bool stopped_early = false;
    std::uint64_t blocks_rendered = 0;
    std::uint64_t late_blocks = 0;
    double peak_render_load = 0.0;
    std::uint64_t non_finite_samples = 0;
    std::uint64_t limited_frames = 0;
    /// Commands pushed to the harness's command ring.
    std::uint64_t commands_sent = 0;
    /// Commands the engine read from it, valid or not, as its status reports.
    std::uint64_t commands_read = 0;
    /// Commands the engine rejected, as its status reports.
    std::uint64_t commands_rejected = 0;
    /// Events that had to wait for room in a ring (requirement 19).
    std::uint64_t events_delayed = 0;
    /// Times the output started freewheeling. Always 0 in file mode.
    std::uint64_t device_losses = 0;
    /// It played through a device, in real time, so late blocks count as a
    /// problem. False in file mode, which renders as fast as it can.
    bool real_time = false;

    bool operator==(const Report&) const = default;
};

/// Whether `report` flags a problem, which makes ks-harness exit with 3:
/// non-finite samples, rejected commands, fewer commands read than sent, or,
/// in real time, late blocks (requirement 28).
[[nodiscard]] bool flags_problem(const Report& report) noexcept;

/// Prints `report` as these lines, with the numbers filled in:
///
///     Played 1.000 s
///     Stopped early                                  (only if it was)
///     Blocks: 188 rendered, 0 late, peak load 0.012
///     Samples: 0 non-finite, 0 limited
///     Commands: 2 sent, 2 read, 0 rejected, 0 delayed
///     Device losses: 0
///
/// "0 delayed" counts events, loads and unloads included.
void print_report(const Report& report, std::ostream& out);

/// File mode's options (requirement 24).
struct FileOptions {
    std::filesystem::path path;
    std::uint32_t sample_rate = 48'000;
    std::uint32_t channels = 2;
    std::uint32_t block_frames = 256;
};

/// Renders `script` with `tracks`, prepared from it, into a 32-bit float WAV
/// file, as fast as possible (requirement 24). Checks `stop` before each
/// block, and stops rendering once it's set. Fails, with a message, if the
/// file can't be written.
[[nodiscard]] std::expected<Report, std::string> render_to_file(const Script& script,
                                                                const TrackSet& tracks,
                                                                const FileOptions& options,
                                                                const std::atomic<bool>& stop);

/// Device mode's options (requirement 25). 0 for the rate, channels, or
/// period: the device's or the backend's choice.
struct DeviceOptions {
    engine::MiniaudioBackends backends = engine::MiniaudioBackends::System;
    /// None: the default device.
    std::optional<engine::DeviceKey> device = std::nullopt;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t period_frames = 0;
};

/// Plays `script` with `tracks`, prepared from it, through a device, in real
/// time (requirements 25 and 26). Writes the device, its state changes, and
/// a status line every 60 seconds to `out`. Once `stop` is set, it stops
/// within 100 ms. Fails, with a message, if no backend initializes or no
/// device can be resolved.
[[nodiscard]] std::expected<Report, std::string>
play_on_device(const Script& script, const TrackSet& tracks, const DeviceOptions& options,
               const std::atomic<bool>& stop, std::ostream& out);

} // namespace ks::harness
