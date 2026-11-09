// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/dsp/frequency_meter.h>

#include <ks/dsp/sine_oscillator.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {

constexpr double kSampleRate = 48000.0;

/// Feeds `seconds` of a sine at `frequency_hz` and `amplitude` to a meter. With
/// `fade_samples > 0`, the gain ramps linearly from 0 over the first and to 0
/// over the last `fade_samples` samples.
double measure(double frequency_hz, double seconds, double amplitude, std::size_t fade_samples) {
    ks::dsp::SineOscillator oscillator{frequency_hz, kSampleRate};
    ks::dsp::FrequencyMeter meter{kSampleRate};
    const auto count = static_cast<std::size_t>(std::lround(seconds * kSampleRate));
    for (std::size_t i = 0; i < count; ++i) {
        double gain = amplitude;
        if (fade_samples > 0) {
            const std::size_t from_edge = std::min(i, count - 1 - i);
            if (from_edge < fade_samples) {
                gain *= static_cast<double>(from_edge) / static_cast<double>(fade_samples);
            }
        }
        meter.push(static_cast<float>(gain * static_cast<double>(oscillator.next())));
    }
    return meter.frequency_hz();
}

} // namespace

TEST_CASE("FrequencyMeter measures sines within 0.05%", "[dsp][frequency_meter]") {
    const double frequency = GENERATE(20.0, 440.0, 440.5, 1000.0, 20000.0);
    const double seconds = GENERATE(0.1, 0.5, 1.0);
    const double amplitude = GENERATE(1.0, 0.001);
    const std::size_t fade_samples = GENERATE(std::size_t{0}, std::size_t{480});

    CAPTURE(frequency, seconds, amplitude, fade_samples);
    const double measured = measure(frequency, seconds, amplitude, fade_samples);
    REQUIRE(std::abs(measured - frequency) <= frequency * 0.0005);
}

TEST_CASE("FrequencyMeter reports 0 for silence", "[dsp][frequency_meter]") {
    ks::dsp::FrequencyMeter meter{kSampleRate};
    REQUIRE(meter.frequency_hz() == 0.0);
    for (int i = 0; i < 48000; ++i) {
        meter.push(0.0F);
    }
    REQUIRE(meter.frequency_hz() == 0.0);
}

TEST_CASE("FrequencyMeter reports 0 with a single crossing", "[dsp][frequency_meter]") {
    ks::dsp::FrequencyMeter meter{kSampleRate};
    meter.push(-1.0F);
    meter.push(1.0F);
    meter.push(1.0F);
    REQUIRE(meter.frequency_hz() == 0.0);
}

TEST_CASE("FrequencyMeter interpolates the crossing position", "[dsp][frequency_meter]") {
    // Crossings at 0.5 and 4.5 samples: one cycle over 4 samples.
    ks::dsp::FrequencyMeter meter{8.0};
    for (const float sample : {-1.0F, 1.0F, 1.0F, 1.0F, -1.0F, 1.0F}) {
        meter.push(sample);
    }
    REQUIRE(meter.frequency_hz() == 2.0);
}
