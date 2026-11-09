// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/dsp/sine_oscillator.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace {

std::vector<float> generate(ks::dsp::SineOscillator& oscillator, std::size_t count) {
    std::vector<float> samples(count);
    std::ranges::generate(samples, [&oscillator] { return oscillator.next(); });
    return samples;
}

} // namespace

TEST_CASE("SineOscillator starts at zero phase", "[dsp][oscillator]") {
    ks::dsp::SineOscillator oscillator{1000.0, 48000.0};
    REQUIRE(oscillator.next() == 0.0F);
}

TEST_CASE("SineOscillator produces the requested frequency", "[dsp][oscillator]") {
    constexpr double kSampleRate = 48000.0;
    constexpr double kFrequency = 1000.0;
    ks::dsp::SineOscillator oscillator{kFrequency, kSampleRate};
    const auto samples = generate(oscillator, static_cast<std::size_t>(kSampleRate)); // one second

    // One upward zero crossing per cycle.
    std::size_t crossings = 0;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        if (samples[i - 1] < 0.0F && samples[i] >= 0.0F) {
            ++crossings;
        }
    }
    REQUIRE(crossings == static_cast<std::size_t>(kFrequency) - 1);
}

TEST_CASE("SineOscillator has unit amplitude", "[dsp][oscillator]") {
    ks::dsp::SineOscillator oscillator{997.0, 48000.0};
    const auto samples = generate(oscillator, 48000);
    const auto [min, max] = std::ranges::minmax(samples);
    REQUIRE(max == Catch::Approx(1.0F).margin(1e-4));
    REQUIRE(min == Catch::Approx(-1.0F).margin(1e-4));
}

TEST_CASE("SineOscillator stays continuous across phase wraps", "[dsp][oscillator]") {
    // 10 s at 997 Hz wraps the phase almost 10,000 times. A wrapping bug would
    // show up as a jump larger than one sample step can produce.
    constexpr double kSampleRate = 48000.0;
    constexpr double kFrequency = 997.0;
    ks::dsp::SineOscillator oscillator{kFrequency, kSampleRate};
    const auto samples = generate(oscillator, 10 * static_cast<std::size_t>(kSampleRate));

    const double max_step = 2.0 * std::numbers::pi * kFrequency / kSampleRate;
    float largest_jump = 0.0F;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        largest_jump = std::max(largest_jump, std::abs(samples[i] - samples[i - 1]));
    }
    REQUIRE(static_cast<double>(largest_jump) <= max_step + 1e-6);
}
