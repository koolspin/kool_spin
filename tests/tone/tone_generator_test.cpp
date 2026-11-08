// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/tone/tone_generator.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr std::size_t kChannels = 2;
constexpr std::size_t kFadeFrames = 480; // 10 ms at 48 kHz: the default fade
const float kAmplitude = static_cast<float>(std::pow(10.0, -20.0 / 20.0)); // -20 dBFS

/// Renders `frames` frames and returns the interleaved samples.
std::vector<float> render(ks::tone::ToneGenerator& generator, std::size_t frames) {
    std::vector<float> buffer(frames * kChannels);
    generator.render(buffer);
    return buffer;
}

float peak_of(const std::vector<float>& samples) {
    float peak = 0.0F;
    for (const float sample : samples) {
        peak = std::max(peak, std::abs(sample));
    }
    return peak;
}

} // namespace

TEST_CASE("ToneGenerator fades in from silence to the requested level", "[tone]") {
    ks::tone::ToneGenerator generator{ks::tone::ToneSettings{}, kSampleRate, kChannels};

    const auto first = render(generator, 1);
    REQUIRE(peak_of(first) == 0.0F);

    const auto fade_in = render(generator, kFadeFrames);
    REQUIRE(peak_of(fade_in) < kAmplitude);

    const auto steady = render(generator, 4800);
    REQUIRE(peak_of(steady) == Catch::Approx(kAmplitude).epsilon(1e-3));
}

TEST_CASE("ToneGenerator writes the same sample to every channel", "[tone]") {
    ks::tone::ToneGenerator generator{ks::tone::ToneSettings{}, kSampleRate, kChannels};
    const auto samples = render(generator, 1024);
    for (std::size_t frame = 0; frame < 1024; ++frame) {
        REQUIRE(samples[frame * kChannels] == samples[(frame * kChannels) + 1]);
    }
}

TEST_CASE("ToneGenerator never exceeds the requested level", "[tone]") {
    ks::tone::ToneGenerator generator{ks::tone::ToneSettings{}, kSampleRate, kChannels};
    const auto samples = render(generator, 48000);
    REQUIRE(peak_of(samples) <= kAmplitude);
    REQUIRE(generator.peak() == peak_of(samples));
}

TEST_CASE("ToneGenerator fades out after request_stop, then plays silence", "[tone]") {
    ks::tone::ToneGenerator generator{ks::tone::ToneSettings{}, kSampleRate, kChannels};
    static_cast<void>(render(generator, 4800));
    REQUIRE_FALSE(generator.finished());

    generator.request_stop();
    const auto fade_out = render(generator, kFadeFrames + 1);
    REQUIRE(generator.finished());
    REQUIRE(fade_out.back() == 0.0F);

    const auto after = render(generator, 1024);
    REQUIRE(peak_of(after) == 0.0F);
}

TEST_CASE("ToneGenerator counts the frames it renders", "[tone]") {
    ks::tone::ToneGenerator generator{ks::tone::ToneSettings{}, kSampleRate, kChannels};
    static_cast<void>(render(generator, 100));
    static_cast<void>(render(generator, 412));
    REQUIRE(generator.frames_rendered() == 512);
}
