// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ks/core/realtime.h>

#include <cassert>
#include <cmath>
#include <numbers>

namespace ks::dsp {

/// Sine wave generator. The phase accumulator is double precision, so the
/// frequency stays exact over long runs.
class SineOscillator {
public:
    /// @pre 0 < frequency_hz < sample_rate / 2
    SineOscillator(double frequency_hz, double sample_rate) noexcept
        : increment_{kTwoPi * frequency_hz / sample_rate} {
        assert(frequency_hz > 0.0 && frequency_hz < sample_rate / 2.0);
    }

    /// Returns the next sample, in [-1, 1]. The first sample is 0.
    [[nodiscard]] float next() noexcept KS_NONBLOCKING {
        const double value = std::sin(phase_);
        phase_ += increment_;
        if (phase_ >= kTwoPi) {
            phase_ -= kTwoPi;
        }
        return static_cast<float>(value);
    }

private:
    static constexpr double kTwoPi = 2.0 * std::numbers::pi;

    double phase_ = 0.0; // radians, in [0, 2π)
    double increment_;   // radians per sample
};

} // namespace ks::dsp
