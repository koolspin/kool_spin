// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the harness's synthetic signals (specs/harness/spec.md
// requirement 23).

#include <ks/harness/script.h>
#include <ks/harness/tracks.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace {

using ks::harness::generate;
using ks::harness::Signal;
using ks::harness::SignalKind;

const double kGain = std::pow(10.0, -12.0 / 20.0);

std::vector<float> left_of(const std::vector<float>& samples) {
    std::vector<float> left;
    for (std::size_t i = 0; i < samples.size(); i += 2) {
        left.push_back(samples.at(i));
    }
    return left;
}

} // namespace

TEST_CASE("Signals: a quarter-rate sine hits the peak on its second frame", "[harness]") {
    const Signal sine{
        .kind = SignalKind::Sine, .frequency_hz = 12'000, .seconds = 0.001, .sample_rate = 48'000};
    const std::vector<float> left = left_of(generate(sine));
    REQUIRE(left.size() == 48);
    CHECK(left.at(0) == 0.0F);
    CHECK(static_cast<double>(left.at(1)) == Catch::Approx(kGain).epsilon(1e-6));
    CHECK(static_cast<double>(left.at(3)) == Catch::Approx(-kGain).epsilon(1e-6));
}

TEST_CASE("Signals: a sweep between equal frequencies is a sine", "[harness]") {
    const Signal sweep{.kind = SignalKind::Sweep,
                       .frequency_hz = 440,
                       .to_hz = 440,
                       .seconds = 0.01,
                       .sample_rate = 44'100};
    const Signal sine{
        .kind = SignalKind::Sine, .frequency_hz = 440, .seconds = 0.01, .sample_rate = 44'100};
    const std::vector<float> a = generate(sweep);
    const std::vector<float> b = generate(sine);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a.at(i) == Catch::Approx(b.at(i)).margin(1e-6));
    }
}

TEST_CASE("Signals: noise stays within the peak and repeats", "[harness]") {
    const Signal noise{.kind = SignalKind::Noise, .seconds = 0.1};
    const std::vector<float> samples = generate(noise);
    REQUIRE(samples.size() == 2 * 4'410);
    const auto limit = static_cast<float>(kGain);
    for (const float sample : samples) {
        CHECK(sample >= -limit);
        CHECK(sample < limit);
    }
    CHECK(samples == generate(noise));
}

TEST_CASE("Signals: clicks start on each beat and are silent between", "[harness]") {
    // 600 BPM at 10 kHz: a beat every 1000 frames, a 100-frame burst.
    const Signal clicks{
        .kind = SignalKind::Clicks, .bpm = 600, .seconds = 0.35, .sample_rate = 10'000};
    const std::vector<float> left = left_of(generate(clicks));
    REQUIRE(left.size() == 3'500);
    const std::span<const float> all{left};
    for (std::size_t beat = 0; beat < left.size(); beat += 1'000) {
        CAPTURE(beat);
        CHECK(all[beat] == 0.0F); // the window is 0 on the beat
        bool sound = false;
        for (const float sample : all.subspan(beat, 100)) {
            sound = sound || sample != 0.0F;
        }
        CHECK(sound);
        const std::size_t gap_end = std::min<std::size_t>(beat + 1'000, left.size());
        for (const float sample : all.subspan(beat + 100, gap_end - (beat + 100))) {
            CHECK(sample == 0.0F);
        }
    }
}

TEST_CASE("Signals: a zero-length signal is empty", "[harness]") {
    CHECK(generate(Signal{.kind = SignalKind::Clicks, .bpm = 120, .seconds = 0}).empty());
}
