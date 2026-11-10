// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Blocks: the frames one call to the render core produces (ADR 0003 §1, §3).

#include <ks/core/realtime.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ks::engine {

/// The most frames in one block. Drivers split larger device callbacks, so
/// the engine's scratch buffers are allocated once, before audio starts.
inline constexpr std::size_t kMaxBlockFrames = 512;

/// The most output channels the engine renders.
inline constexpr std::size_t kMaxOutputChannels = 8;

/// What one call to the render core renders.
///
/// A block is valid when it has 1 to kMaxBlockFrames frames, 1 to
/// kMaxOutputChannels channels, and a nonzero sample rate, and every channel
/// holds at least `frames` samples. Only the first `frames` samples of each
/// channel belong to the block.
struct Block {
    /// The output, one buffer per channel.
    std::span<const std::span<float>> channels;
    std::size_t frames = 0;
    std::uint32_t sample_rate = 0;
    /// The monotonic time of the block's first frame: when the device asked
    /// for it, or a synthetic time when rendering offline.
    std::chrono::steady_clock::time_point host_time;
};

/// The duration of `frames` frames at `sample_rate`: floor(frames × 10⁹ /
/// sample_rate) nanoseconds, exactly, for up to about 292 years of audio.
/// Returns 0 if `sample_rate` is 0.
[[nodiscard]] std::chrono::nanoseconds
frames_to_duration(std::uint64_t frames, std::uint32_t sample_rate) noexcept KS_NONBLOCKING;

} // namespace ks::engine
