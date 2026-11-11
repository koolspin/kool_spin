// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/interpolator.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace ks::engine {

namespace {

constexpr std::size_t kTablePoints =
    static_cast<std::size_t>(Interpolator::kHalfWidth) * Interpolator::kPointsPerFrame;

/// The zeroth-order modified Bessel function of the first kind, by its
/// power series, which converges quickly for the arguments the window uses.
double bessel_i0(double x) {
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; term > sum * 1e-17; ++k) {
        const double half = x / (2.0 * k);
        term *= half * half;
        sum += term;
    }
    return sum;
}

/// h₁(t) = sinc(t) · w(t / kHalfWidth), at table point `point`.
double kernel(std::size_t point) {
    if (point == 0) {
        return 1.0;
    }
    if (point % Interpolator::kPointsPerFrame == 0) {
        return 0.0; // the other integers, where sinc is exactly 0
    }
    const double t = static_cast<double>(point) / Interpolator::kPointsPerFrame;
    const double sinc = std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
    const double v = t / Interpolator::kHalfWidth;
    const double window = bessel_i0(Interpolator::kKaiserBeta * std::sqrt(1.0 - (v * v))) /
                          bessel_i0(Interpolator::kKaiserBeta);
    return sinc * window;
}

} // namespace

Interpolator::Interpolator() : table_(kTablePoints + 2, 0.0F) {
    // Points 0 to kTablePoints; the last is t = kHalfWidth, where sinc is 0.
    // One more 0 after it lets a lookup interpolate without a bounds check.
    for (std::size_t point = 0; point <= kTablePoints; ++point) {
        table_[point] = static_cast<float>(kernel(point));
    }
}

StereoSample Interpolator::value(std::span<const float> samples, std::uint64_t ready_frames,
                                 TrackPosition position,
                                 double stretch) const noexcept KS_NONBLOCKING {
    const double s = std::clamp(stretch, 1.0, kMaxStretch);
    const auto ready =
        static_cast<std::int64_t>(std::min<std::uint64_t>(ready_frames, samples.size() / 2));

    // The position is n + f: n is the frame at or before it, and f is in [0, 1).
    const std::int64_t n = position >> kPositionFractionBits;
    const double f =
        static_cast<double>(position & (kOneFrame - 1)) / static_cast<double>(kOneFrame);
    // Table points per frame of distance: the stretch widens the kernel.
    const double scale = kPointsPerFrame / s;
    const auto gain = static_cast<float>(1.0 / s);
    const auto taps = static_cast<std::int64_t>(std::ceil(kHalfWidth * s));

    // Frames n + j for j in [1 - taps, taps] cover every frame within the
    // kernel's half-width, kHalfWidth × s, of the position.
    const std::span<const float> table{table_};
    float left = 0.0F;
    float right = 0.0F;
    for (std::int64_t j = 1 - taps; j <= taps; ++j) {
        const std::int64_t k = n + j;
        if (k < 0 || k >= ready) {
            continue; // reads as 0
        }
        const double u = std::abs(static_cast<double>(j) - f) * scale;
        const auto point = static_cast<std::size_t>(u);
        if (point >= kTablePoints) {
            continue; // outside the kernel
        }
        const auto fraction = static_cast<float>(u - static_cast<double>(point));
        const float h0 = table[point];
        const float weight = (h0 + ((table[point + 1] - h0) * fraction)) * gain;
        const auto frame = static_cast<std::size_t>(k) * 2;
        left += weight * samples[frame];
        right += weight * samples[frame + 1];
    }
    return StereoSample{.left = left, .right = right};
}

} // namespace ks::engine
