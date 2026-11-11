// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/deck/spec.md: the interpolator, through a deck.

#include <ks/engine/deck.h>
#include <ks/engine/interpolator.h>
#include <ks/engine/track.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

namespace {

using ks::engine::Deck;
using ks::engine::Interpolator;
using ks::engine::test::fit_tone;
using ks::engine::test::kNaN;
using ks::engine::test::kRampFrames;
using ks::engine::test::kRate;
using ks::engine::test::kSignal;
using ks::engine::test::level_db;
using ks::engine::test::OwnedTrack;
using ks::engine::test::render;
using ks::engine::test::render_tracking;
using ks::engine::test::sine;
using ks::engine::test::Stereo;

/// Where tone measurements start: after the play ramp, with margin.
constexpr std::size_t kFitStart = 2 * kRampFrames;
/// How many frames tone measurements use.
constexpr std::size_t kFitFrames = 4'800;

/// Plays a sine of `frequency` Hz from a track at `track_rate`, at `rate`, on
/// a device at `device_rate`, starting at track frame `start`.
Stereo play_sine(const Interpolator& interpolator, double frequency, std::uint32_t track_rate,
                 std::uint32_t device_rate, double rate, double start = 1'000.0) {
    OwnedTrack owned{40'000, track_rate, sine(frequency, track_rate)};
    Deck deck{interpolator};
    REQUIRE(deck.load(owned.track()));
    deck.set_rate(rate);
    deck.seek(start);
    deck.set_playing(true);
    return render(deck, kFitStart + kFitFrames, 64, device_rate);
}

/// Radians per frame of `frequency` at `sample_rate`.
double omega(double frequency, std::uint32_t sample_rate) {
    return 2.0 * std::numbers::pi * frequency / static_cast<double>(sample_rate);
}

/// The fit window of one channel.
std::span<const float> window(const Stereo& out, std::size_t channel) {
    return std::span{channel == 0 ? out.left : out.right}.subspan(kFitStart, kFitFrames);
}

/// Checks a sine came out at `frequency`, at amplitude 0.5 ± 0.01 dB, with a
/// residual of at most -80 dB.
void check_tone(const Stereo& out, double frequency, std::uint32_t device_rate) {
    for (std::size_t channel = 0; channel < 2; ++channel) {
        CAPTURE(channel);
        const auto fit = fit_tone(window(out, channel), omega(frequency, device_rate));
        const double level = 20.0 * std::log10(fit.amplitude / 0.5);
        CAPTURE(level, fit.residual_db);
        REQUIRE(std::abs(level) <= 0.01);
        REQUIRE(fit.residual_db <= -80.0);
    }
}

} // namespace

TEST_CASE("Interpolation: unity rate is bit-exact", "[deck][interpolation]") {
    const Interpolator interpolator;
    OwnedTrack owned{20'000, kRate, kSignal};
    Deck deck{interpolator};
    REQUIRE(deck.load(owned.track()));
    deck.seek(1'000.0);
    deck.set_playing(true);
    const Stereo out = render_tracking(deck, 4'000);

    for (std::size_t frame = 0; frame < 4'000; ++frame) {
        CAPTURE(frame);
        REQUIRE(out.positions.at(frame) == 1'000.0 + static_cast<double>(frame));
    }
    for (std::size_t channel = 0; channel < 2; ++channel) {
        for (std::size_t frame = kRampFrames; frame < 4'000; ++frame) {
            CAPTURE(channel, frame);
            REQUIRE(std::bit_cast<std::uint32_t>(out.at(channel, frame)) ==
                    std::bit_cast<std::uint32_t>(owned.sample(1'000 + frame, channel)));
        }
    }
}

TEST_CASE("Interpolation: converting the track's rate", "[deck][interpolation]") {
    const Interpolator interpolator;
    for (const double frequency : {1'000.0, 10'000.0, 18'000.0}) {
        CAPTURE(frequency);
        check_tone(play_sine(interpolator, frequency, 44'100, 48'000, 1.0), frequency, 48'000);
    }
    for (const double frequency : {1'000.0, 10'000.0}) {
        CAPTURE(frequency);
        check_tone(play_sine(interpolator, frequency, 48'000, 44'100, 1.0), frequency, 44'100);
    }
}

TEST_CASE("Interpolation: playing faster doesn't alias", "[deck][interpolation]") {
    const Interpolator interpolator;
    check_tone(play_sine(interpolator, 5'000.0, 44'100, 48'000, 2.0), 10'000.0, 48'000);

    // 15 kHz at rate 2 would be 30 kHz, above the output's 24 kHz.
    const Stereo above = play_sine(interpolator, 15'000.0, 44'100, 48'000, 2.0);
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const double level = level_db(window(above, channel), 0.5);
        CAPTURE(channel, level);
        REQUIRE(level <= -80.0);
    }
}

TEST_CASE("Interpolation: reverse", "[deck][interpolation]") {
    const Interpolator interpolator;
    OwnedTrack owned{40'000, 44'100, sine(1'000.0, 44'100)};
    Deck deck{interpolator};
    REQUIRE(deck.load(owned.track()));
    deck.set_rate(-1.0);
    deck.seek(30'000.0);
    deck.set_playing(true);
    const Stereo out = render(deck, kFitStart + kFitFrames);
    check_tone(out, 1'000.0, 48'000);
    REQUIRE(deck.position() < 30'000.0 - 4'000.0);
}

TEST_CASE("Interpolation: reads only decoded frames", "[deck][interpolation]") {
    const Interpolator interpolator;
    constexpr std::uint64_t kCapacity = 48'000;
    constexpr std::size_t kGuard = 1'000;
    // Frames that aren't ready hold NaN, and so does the memory around the
    // track: reading any of it would make the output NaN.
    const auto ready_up_to = [](std::uint64_t ready) {
        return [ready](std::uint64_t frame, std::size_t channel) {
            return frame < ready ? kSignal(frame, channel) : kNaN;
        };
    };

    SECTION("into the waiting limit, at rates up to 4") {
        for (const double rate : {1.0, 2.0, 4.0}) {
            CAPTURE(rate);
            OwnedTrack owned{kCapacity, kRate, ready_up_to(10'000), kGuard};
            owned.track().publish(10'000, false);
            Deck deck{interpolator};
            REQUIRE(deck.load(owned.track()));
            deck.set_rate(rate);
            deck.seek(9'000.0);
            deck.set_playing(true);
            REQUIRE(render(deck, kRate).finite());
            REQUIRE(deck.waiting());
        }
    }

    SECTION("past the ready frames") {
        OwnedTrack owned{kCapacity, kRate, ready_up_to(10'000), kGuard};
        owned.track().publish(10'000, false);
        Deck deck{interpolator};
        REQUIRE(deck.load(owned.track()));
        deck.seek(20'000.0);
        deck.set_playing(true);
        REQUIRE(render(deck, kRate).finite());
    }

    SECTION("in reverse into frame 0") {
        OwnedTrack owned{kCapacity, kRate, kSignal, kGuard};
        Deck deck{interpolator};
        REQUIRE(deck.load(owned.track()));
        deck.set_rate(-4.0);
        deck.seek(500.0);
        deck.set_playing(true);
        REQUIRE(render(deck, kRate).finite());
        REQUIRE(deck.position() == 0.0);
    }

    SECTION("a complete track into its end") {
        OwnedTrack owned{kCapacity, kRate, ready_up_to(30'000), kGuard};
        owned.track().publish(30'000, true);
        Deck deck{interpolator};
        REQUIRE(deck.load(owned.track()));
        deck.set_rate(4.0);
        deck.seek(29'000.0);
        deck.set_playing(true);
        REQUIRE(render(deck, kRate).finite());
        REQUIRE(deck.position() == 30'000.0);
    }
}
