// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ks/core/realtime.h>

#include <cstdint>

namespace ks::dsp {

/// Measures the frequency of a periodic signal from its upward zero crossings.
///
/// Each crossing's position is interpolated linearly between the two samples
/// around it. The frequency comes from the span between the first and the last
/// crossing, so errors at individual crossings don't accumulate, and gain
/// changes (fades) don't move the crossings. Holds only a few scalars: safe to
/// use on the audio thread.
class FrequencyMeter {
public:
    /// @pre sample_rate > 0
    explicit FrequencyMeter(double sample_rate) noexcept : sample_rate_{sample_rate} {}

    /// Feeds the next sample.
    void push(float sample) noexcept KS_NONBLOCKING {
        if (has_previous_ && previous_ <= 0.0F && sample > 0.0F) {
            // Fraction of the way from the previous sample to this one where the
            // signal crosses zero, in [0, 1).
            const auto previous = static_cast<double>(previous_);
            const double fraction = -previous / (static_cast<double>(sample) - previous);
            const double position = static_cast<double>(index_ - 1) + fraction;
            if (crossings_ == 0) {
                first_crossing_ = position;
            }
            last_crossing_ = position;
            ++crossings_;
        }
        previous_ = sample;
        has_previous_ = true;
        ++index_;
    }

    /// Returns the measured frequency in Hz, or 0 with fewer than two upward
    /// zero crossings so far.
    [[nodiscard]] double frequency_hz() const noexcept KS_NONBLOCKING {
        if (crossings_ < 2 || !(last_crossing_ > first_crossing_)) {
            return 0.0;
        }
        return static_cast<double>(crossings_ - 1) * sample_rate_ /
               (last_crossing_ - first_crossing_);
    }

private:
    double sample_rate_;
    std::uint64_t index_ = 0;     // index of the next sample to be pushed
    std::uint64_t crossings_ = 0; // upward zero crossings seen
    double first_crossing_ = 0.0; // sample position of the first crossing
    double last_crossing_ = 0.0;  // sample position of the latest crossing
    float previous_ = 0.0F;
    bool has_previous_ = false;
};

} // namespace ks::dsp
