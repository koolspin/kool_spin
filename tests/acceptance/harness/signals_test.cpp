// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/harness/spec.md: synthetic signals
// (requirement 23).

#include <ks/harness/script.h>
#include <ks/harness/tracks.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using ks::harness::generate;
using ks::harness::Signal;
using ks::harness::SignalKind;
using ks::harness::test::channel;
using ks::harness::test::dbfs;
using ks::harness::test::frequency;
using ks::harness::test::kPeak;
using ks::harness::test::measure;
using ks::harness::test::Measured;
using ks::harness::test::peak;
using ks::harness::test::within;

/// Checks the length, that both channels are the same, and returns the left.
std::vector<float> checked_mono(const Signal& signal) {
    const std::vector<float> samples = generate(signal);
    const auto frames = static_cast<std::size_t>(
        std::llround(signal.seconds * static_cast<double>(signal.sample_rate)));
    REQUIRE(samples.size() == 2 * frames);
    const std::vector<float> left = channel(samples, 2, 0);
    const std::vector<float> right = channel(samples, 2, 1);
    CHECK(left == right);
    return left;
}

/// Whether the peak is −12 dBFS within 0.1 dB.
bool peaks_at_minus_12(std::span<const float> samples) {
    const double level = dbfs(peak(samples));
    return level >= -12.1 && level <= -11.9;
}

} // namespace

TEST_CASE("Signals: sine, sweep, noise, and clicks", "[harness]") {
    SECTION("sine: its frequency, from phase 0") {
        const Signal sine{
            .kind = SignalKind::Sine, .frequency_hz = 1000, .seconds = 1, .sample_rate = 48'000};
        const std::vector<float> left = checked_mono(sine);
        CHECK(peaks_at_minus_12(left));
        CHECK(std::abs(left.at(0)) < 1e-6F);
        CHECK(left.at(1) > 0.0F);
        const double measured = frequency(left, 48'000, 0, left.size());
        CAPTURE(measured);
        CHECK(within(measured, 1000.0, 0.01));
    }

    SECTION("sine: the default rate, and an odd length") {
        const Signal sine{.kind = SignalKind::Sine, .frequency_hz = 440, .seconds = 0.3};
        const std::vector<float> left = checked_mono(sine); // 13230 frames at 44.1 kHz
        CHECK(left.size() == 13'230);
        CHECK(peaks_at_minus_12(left));
    }

    SECTION("sweep: exponential, from one frequency to the other") {
        constexpr std::uint32_t kRate = 44'100;
        const Signal sweep{.kind = SignalKind::Sweep,
                           .frequency_hz = 100,
                           .to_hz = 10'000,
                           .seconds = 2,
                           .sample_rate = kRate};
        const std::vector<float> left = checked_mono(sweep);
        CHECK(peaks_at_minus_12(left));

        // The frequency over a short window, against the formula where it
        // was measured.
        const auto expected_at = [](double t) { return 100.0 * std::pow(100.0, t / 2.0); };
        const auto check_window = [&](double start_s, double length_s) {
            const auto first = static_cast<std::size_t>(start_s * kRate);
            const auto last = static_cast<std::size_t>((start_s + length_s) * kRate);
            const Measured measured = measure(left, kRate, first, last);
            const double expected = expected_at(measured.at_seconds);
            CAPTURE(start_s, measured.hz, measured.at_seconds, expected);
            CHECK(within(measured.hz, expected, 0.01));
        };
        check_window(0.0, 0.03);      // the start: 3 cycles
        check_window(0.995, 0.01);    // the middle: about 10 cycles
        check_window(1.9985, 0.0015); // the end: about 15 cycles
    }

    SECTION("noise: the same on every run") {
        const Signal noise{.kind = SignalKind::Noise, .seconds = 1};
        const std::vector<float> left = checked_mono(noise);
        CHECK(peaks_at_minus_12(left));
        CHECK(generate(noise) == generate(noise));

        // It's noise, not a constant: about zero on average, and spread out.
        double sum = 0.0;
        double squares = 0.0;
        for (const float sample : left) {
            sum += static_cast<double>(sample);
            squares += static_cast<double>(sample) * static_cast<double>(sample);
        }
        const auto count = static_cast<double>(left.size());
        CHECK(std::abs(sum / count) < 0.01);
        const double rms = std::sqrt(squares / count);
        CHECK(rms > kPeak / 3.0); // uniform noise: rms = peak / sqrt(3)
    }

    SECTION("clicks: 10 ms bursts on the beats, silent between") {
        constexpr std::uint32_t kRate = 48'000;
        const Signal clicks{
            .kind = SignalKind::Clicks, .bpm = 120, .seconds = 2, .sample_rate = kRate};
        const std::vector<float> left = checked_mono(clicks);
        CHECK(peak(left) <= kPeak + 1e-6);

        constexpr std::size_t kBeat = kRate / 2;    // 120 BPM
        constexpr std::size_t kBurst = kRate / 100; // 10 ms
        const std::span<const float> all{left};
        for (std::size_t beat = 0; beat < left.size(); beat += kBeat) {
            CAPTURE(beat);
            const std::span<const float> burst = all.subspan(beat, kBurst);
            CHECK(peak(burst) > kPeak / 2.0);
            // It starts on the beat: the window is 0 at its first frame, so
            // sound starts within 2 frames.
            const auto first_sound = static_cast<std::size_t>(
                std::ranges::find_if(burst, [](float x) { return x != 0.0F; }) - burst.begin());
            CAPTURE(first_sound);
            CHECK(first_sound <= 2);
            // The Hann window peaks in the burst's middle, so a burst that
            // starts late peaks late: the loudest sample is within half a
            // 1 kHz cycle (24 frames) of the middle.
            const auto loudest = static_cast<std::size_t>(
                std::ranges::max_element(burst, {}, [](float x) { return std::abs(x); }) -
                burst.begin());
            CAPTURE(loudest);
            CHECK(loudest >= (kBurst / 2) - 30);
            CHECK(loudest <= (kBurst / 2) + 30);
            const std::size_t gap_end = std::min(beat + kBeat, left.size());
            CHECK(peak(all.subspan(beat + kBurst, gap_end - (beat + kBurst))) == 0.0);
        }
    }
}
