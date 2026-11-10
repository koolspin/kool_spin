// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Interleaving, and a device driver's work for one callback.

#include <ks/core/realtime.h>
#include <ks/engine/render_core.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ks::engine {

/// Writes sample f of channel c of `planar` to `interleaved[f * channels + c]`,
/// for the first `frames` frames. Writes nothing and returns false if
/// `interleaved` holds fewer than `frames * planar.size()` samples, or a
/// channel holds fewer than `frames`.
[[nodiscard]] bool interleave(std::span<const std::span<float>> planar, std::size_t frames,
                              std::span<float> interleaved) noexcept KS_NONBLOCKING;

/// Renders `interleaved.size() / channels` frames into `interleaved`, as a
/// device driver does for one callback: in blocks of at most kMaxBlockFrames,
/// each rendered into the first `channels` scratch buffers and interleaved
/// into place. The block that starts at frame k has host time `host_time` +
/// frames_to_duration(k, sample_rate). Returns the number of frames in
/// rendered blocks.
///
/// If the arguments are inconsistent, fills `interleaved` with silence and
/// returns 0. They're inconsistent when `channels` is 0 or more than
/// kMaxOutputChannels, `scratch` has fewer than `channels` buffers or one of
/// them holds fewer than kMaxBlockFrames samples, `interleaved.size()` isn't a
/// multiple of `channels`, or the sample rate is 0.
std::size_t
render_interleaved(RenderCore& core, std::span<const std::span<float>> scratch,
                   std::span<float> interleaved, std::size_t channels, std::uint32_t sample_rate,
                   std::chrono::steady_clock::time_point host_time) noexcept KS_NONBLOCKING;

} // namespace ks::engine
