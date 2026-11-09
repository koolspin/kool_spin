// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/tone/tone_generator.h>

#include <algorithm>
#include <cmath>

namespace ks::tone {
namespace {

float dbfs_to_amplitude(double dbfs) noexcept {
    return static_cast<float>(std::pow(10.0, dbfs / 20.0));
}

/// Gain change per frame for a linear fade between silence and full level.
float fade_step(double fade_seconds, double sample_rate) noexcept {
    return static_cast<float>(1.0 / (fade_seconds * sample_rate));
}

} // namespace

ToneGenerator::ToneGenerator(const ToneSettings& settings, double sample_rate,
                             std::size_t channels) noexcept
    : oscillator_{settings.frequency_hz, sample_rate}, channels_{channels},
      amplitude_{dbfs_to_amplitude(settings.level_dbfs)},
      gain_step_{fade_step(settings.fade_seconds, sample_rate)}, meter_{sample_rate} {}

void ToneGenerator::render(std::span<float> interleaved) noexcept KS_NONBLOCKING {
    if (!fading_out_ && stop_requested_.load(std::memory_order_acquire)) {
        fading_out_ = true;
    }

    const std::size_t frames = interleaved.size() / channels_;
    float peak = peak_.load(std::memory_order_relaxed);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        gain_ =
            fading_out_ ? std::max(0.0F, gain_ - gain_step_) : std::min(1.0F, gain_ + gain_step_);
        const float sample = gain_ > 0.0F ? oscillator_.next() * amplitude_ * gain_ : 0.0F;
        for (std::size_t channel = 0; channel < channels_; ++channel) {
            interleaved[(frame * channels_) + channel] = sample;
        }
        peak = std::max(peak, std::abs(sample));
        meter_.push(sample); // channel 0; every channel is identical
    }

    peak_.store(peak, std::memory_order_relaxed);
    frames_rendered_.fetch_add(frames, std::memory_order_relaxed);
    measured_hz_.store(meter_.frequency_hz(), std::memory_order_release);
    if (fading_out_ && gain_ <= 0.0F) {
        finished_.store(true, std::memory_order_release);
    }
}

void ToneGenerator::request_stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
}

bool ToneGenerator::finished() const noexcept {
    return finished_.load(std::memory_order_acquire);
}

std::uint64_t ToneGenerator::frames_rendered() const noexcept {
    return frames_rendered_.load(std::memory_order_relaxed);
}

float ToneGenerator::peak() const noexcept {
    return peak_.load(std::memory_order_relaxed);
}

double ToneGenerator::measured_frequency_hz() const noexcept {
    return measured_hz_.load(std::memory_order_acquire);
}

} // namespace ks::tone
