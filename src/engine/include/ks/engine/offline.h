// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The offline driver: renders into memory, as fast as possible, on the
/// caller's thread (ADR 0003 §1). Golden and property tests use it.

#include <ks/core/realtime.h>
#include <ks/engine/render_core.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ks::engine {

struct OfflineConfig {
    std::uint32_t sample_rate = 0;
    /// Frames per block. Blocks larger than kMaxBlockFrames are split.
    std::size_t block_frames = 0;
    /// The host time of the first frame. Later frames follow at exactly the
    /// sample rate.
    std::chrono::steady_clock::time_point start_time;
};

/// Renders every frame of `output`, one buffer per channel, in order, in
/// blocks of `config.block_frames` (split into pieces of at most
/// kMaxBlockFrames). Blocks another driver's render token skips are silent.
/// Returns the number of frames in rendered blocks.
///
/// Renders nothing, leaves `output` untouched, and returns 0 if `output` has
/// no channels or more than kMaxOutputChannels, its channels differ in
/// length, or the block size or sample rate is 0.
std::size_t render_offline(RenderCore& core, const OfflineConfig& config,
                           std::span<const std::span<float>> output) noexcept KS_NONBLOCKING;

} // namespace ks::engine
