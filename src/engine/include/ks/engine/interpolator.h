// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The deck's resampler: a windowed-sinc kernel that plays a track at any
/// rate, and converts its sample rate to the device's in the same step
/// (specs/deck/spec.md, requirements 3 to 6).

#include <ks/core/realtime.h>

#include <cstdint>
#include <span>
#include <vector>

namespace ks::engine {

/// A position in track frames, in fixed point: frames × 2³².
using TrackPosition = std::int64_t;

/// The fractional bits of a TrackPosition.
inline constexpr int kPositionFractionBits = 32;

/// One frame, as a TrackPosition.
inline constexpr TrackPosition kOneFrame = TrackPosition{1} << kPositionFractionBits;

/// The farthest the interpolator reads from a position, in frames.
inline constexpr std::int64_t kInterpolatorReach = 128;

/// One stereo frame.
struct StereoSample {
    float left = 0.0F;
    float right = 0.0F;
};

/// The kernel table. Construct it off the audio thread, since it allocates;
/// after that it never changes, and any number of decks can share it.
class Interpolator {
public:
    /// The kernel's half-width, in frames, at a stretch of 1.
    static constexpr int kHalfWidth = 32;
    /// The most the kernel widens, when playing faster than the output rate.
    static constexpr double kMaxStretch = 4.0;
    /// Table points per frame of the kernel.
    static constexpr int kPointsPerFrame = 512;
    /// The Kaiser window's β.
    static constexpr double kKaiserBeta = 8.0;

    /// Builds the table.
    Interpolator();

    /// The value of a track at `position`. `samples` is interleaved stereo;
    /// frames below 0, or at or beyond `ready_frames`, read as 0, and no other
    /// memory is read. `stretch` is clamped to [1, kMaxStretch]: the kernel's
    /// cutoff is lowered, and its width increased, by that factor.
    [[nodiscard]] StereoSample value(std::span<const float> samples, std::uint64_t ready_frames,
                                     TrackPosition position,
                                     double stretch) const noexcept KS_NONBLOCKING;

private:
    /// h₁(t) at t = i / kPointsPerFrame, for i from 0 to kHalfWidth ×
    /// kPointsPerFrame, plus a trailing 0.
    std::vector<float> table_;
};

} // namespace ks::engine
