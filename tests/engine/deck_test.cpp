// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/deck.h>

#include <ks/engine/block.h>
#include <ks/engine/interpolator.h>
#include <ks/engine/track.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using ks::engine::Deck;
using ks::engine::Interpolator;
using ks::engine::kInterpolatorReach;
using ks::engine::kMaxBlockFrames;
using ks::engine::kOneFrame;
using ks::engine::StereoSample;
using ks::engine::Track;
using ks::engine::TrackPosition;

constexpr std::uint32_t kRate = 48'000;

/// Interleaved stereo frames: left is frame + 1, right is -(frame + 1).
std::vector<float> ramp_samples(std::size_t frames) {
    std::vector<float> samples(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        samples.at(frame * 2) = static_cast<float>(frame + 1);
        samples.at((frame * 2) + 1) = -static_cast<float>(frame + 1);
    }
    return samples;
}

/// Renders `frames` frames of the left channel, in blocks of 64.
std::vector<float> render_left(Deck& deck, std::size_t frames, std::uint32_t sample_rate = kRate) {
    std::vector<float> left(frames);
    std::vector<float> right(frames);
    for (std::size_t done = 0; done < frames; done += 64) {
        const std::size_t n = std::min<std::size_t>(64, frames - done);
        deck.render(std::span{left}.subspan(done, n), std::span{right}.subspan(done, n),
                    sample_rate);
    }
    return left;
}

TrackPosition at(double frame) {
    return static_cast<TrackPosition>(frame * static_cast<double>(kOneFrame));
}

} // namespace

TEST_CASE("Track: progress is clamped to the capacity", "[engine][deck]") {
    const std::vector<float> samples(200);
    Track track{samples, kRate};
    REQUIRE(track.capacity() == 100);
    REQUIRE(track.ready().frames == 0);
    REQUIRE_FALSE(track.ready().complete);

    track.publish(40, false);
    REQUIRE(track.ready().frames == 40);
    REQUIRE_FALSE(track.ready().complete);

    track.publish(1'000, true);
    REQUIRE(track.ready().frames == 100);
    REQUIRE(track.ready().complete);

    // The top bit is the complete flag; a count that reaches it can't set it.
    track.publish(std::uint64_t{1} << 63U, false);
    REQUIRE_FALSE(track.ready().complete);
}

TEST_CASE("Interpolator: integer positions read the samples exactly", "[engine][deck]") {
    const Interpolator interpolator;
    const std::vector<float> samples = ramp_samples(100);
    for (const std::int64_t frame : {0, 1, 50, 99}) {
        CAPTURE(frame);
        const StereoSample value =
            interpolator.value(samples, 100, at(static_cast<double>(frame)), 1.0);
        REQUIRE(value.left == static_cast<float>(frame + 1));
        REQUIRE(value.right == -static_cast<float>(frame + 1));
    }
}

TEST_CASE("Interpolator: DC passes at every fraction and stretch", "[engine][deck]") {
    const Interpolator interpolator;
    const std::vector<float> ones(2'000, 1.0F);
    for (const double stretch : {0.5, 1.0, 1.7, 4.0, 9.0}) {
        for (const double fraction : {0.0, 0.1, 0.25, 0.5, 0.999}) {
            CAPTURE(stretch, fraction);
            const StereoSample value =
                interpolator.value(ones, 1'000, at(500.0 + fraction), stretch);
            REQUIRE(std::abs(value.left - 1.0F) < 1e-3F);
        }
    }
}

TEST_CASE("Interpolator: outside the ready frames reads as silence", "[engine][deck]") {
    const Interpolator interpolator;
    const std::vector<float> samples = ramp_samples(100);
    // Far before the start, far past the ready frames, and nothing ready.
    REQUIRE(interpolator.value(samples, 100, at(-500.0), 1.0).left == 0.0F);
    REQUIRE(interpolator.value(samples, 50, at(50.0 + kInterpolatorReach), 4.0).left == 0.0F);
    REQUIRE(interpolator.value(samples, 0, at(10.5), 1.0).left == 0.0F);
    // A ready count beyond the memory is clamped to it.
    REQUIRE(std::isfinite(interpolator.value(samples, 1'000'000, at(99.5), 1.0).left));
}

TEST_CASE("Deck: invalid renders change nothing", "[engine][deck]") {
    const Interpolator interpolator;
    const std::vector<float> samples = ramp_samples(1'000);
    Track track{samples, kRate};
    track.publish(1'000, true);
    Deck deck{interpolator};
    REQUIRE(deck.load(track));
    deck.set_playing(true);

    std::vector<float> left(kMaxBlockFrames + 1, 5.0F);
    std::vector<float> right(kMaxBlockFrames + 1, 5.0F);
    const std::span<float> l{left};
    const std::span<float> r{right};
    deck.render(l.first(10), r.first(9), kRate);
    deck.render(l.first(0), r.first(0), kRate);
    deck.render(l, r, kRate);
    deck.render(l.first(10), r.first(10), 0);
    REQUIRE(left.front() == 5.0F);
    REQUIRE(left.back() == 5.0F);
    REQUIRE(deck.position() == 0.0);
}

TEST_CASE("Deck: ramps follow the device's sample rate", "[engine][deck]") {
    const Interpolator interpolator;
    const std::vector<float> ones(200'000, 1.0F);
    Track track{ones, 44'100};
    track.publish(100'000, true);
    Deck deck{interpolator};
    REQUIRE(deck.load(track));
    deck.seek(1'000.0);
    deck.set_playing(true);

    // At 44.1 kHz, 5 ms is 220.5 frames, rounded to 221. The track is at the
    // same rate, so positions stay on whole frames and the values are exact.
    const std::vector<float> out = render_left(deck, 300, 44'100);
    REQUIRE(std::abs(out.at(1) - (1.0F / 221.0F)) < 1e-6F);
    REQUIRE(out.at(221) == 1.0F);

    // At 8 kHz, a full ramp of 221 becomes 40, the new maximum. The values
    // are now interpolated, so they're 1 only to within the kernel's ripple.
    deck.set_playing(false);
    const std::vector<float> slow = render_left(deck, 50, 8'000);
    REQUIRE(std::abs(slow.at(0) - 1.0F) < 1e-3F);
    REQUIRE(std::abs(slow.at(1) - (39.0F / 40.0F)) < 1e-3F);
    REQUIRE(slow.at(45) == 0.0F);
}

TEST_CASE("Deck: crossfading between tracks at different rates", "[engine][deck]") {
    const Interpolator interpolator;
    const std::vector<float> ones(100'000, 1.0F);
    Track fast{ones, kRate};
    fast.publish(50'000, true);
    Track slow{ones, 24'000};
    slow.publish(50'000, true);
    Deck deck{interpolator};
    REQUIRE(deck.load(fast));
    deck.set_playing(true);
    static_cast<void>(render_left(deck, 1'000));
    REQUIRE(deck.position() == 1'000.0);

    // The new track steps half a frame per frame; the old one keeps its own
    // step while it fades out.
    REQUIRE(deck.load(slow));
    deck.set_playing(true);
    static_cast<void>(render_left(deck, 1'000));
    REQUIRE(deck.position() == 500.0);
    REQUIRE_FALSE(deck.holds(fast));
}

TEST_CASE("Deck: controls during a crossfade wait in order", "[engine][deck]") {
    const Interpolator interpolator;
    const std::vector<float> samples = ramp_samples(50'000);
    Track first{samples, kRate};
    first.publish(50'000, true);
    Track second{samples, kRate};
    second.publish(50'000, true);
    Deck deck{interpolator};
    REQUIRE(deck.load(first));
    deck.set_playing(true);
    static_cast<void>(render_left(deck, 1'000));

    deck.seek(10'000.0); // starts a crossfade
    static_cast<void>(render_left(deck, 10));

    SECTION("a load, then a seek and play, apply after it, in that order") {
        REQUIRE(deck.load(second));
        REQUIRE(deck.holds(second));
        REQUIRE(deck.track() == &first);
        deck.seek(2'000.0);
        deck.set_playing(true);
        deck.set_cue(30.0);
        REQUIRE(deck.cue() == 30.0);

        static_cast<void>(render_left(deck, 230)); // the first crossfade ends
        REQUIRE(deck.track() == &second);
        static_cast<void>(render_left(deck, 500)); // and the second
        REQUIRE(deck.position() == 2'000.0 + 500.0);
        REQUIRE(deck.playing());
        REQUIRE_FALSE(deck.holds(first));
    }

    SECTION("an unload stops the deck once it applies") {
        deck.unload();
        deck.set_playing(true); // the waiting unload leaves no track to play
        REQUIRE_FALSE(deck.playing());
        static_cast<void>(render_left(deck, 1'000));
        REQUIRE(deck.track() == nullptr);
        REQUIRE_FALSE(deck.holds(first));
        REQUIRE_FALSE(deck.playing());
    }

    SECTION("a seek when the main head has gone silent applies at once") {
        deck.set_playing(false);
        static_cast<void>(render_left(deck, 20)); // still crossfading
        deck.seek(20'000.0);
        REQUIRE(deck.position() == 20'000.0);
    }
}

TEST_CASE("Deck: a track with less ready than the reach waits at its start", "[engine][deck]") {
    const Interpolator interpolator;
    const std::vector<float> samples = ramp_samples(10'000);
    Track track{samples, kRate};
    track.publish(50, false);
    Deck deck{interpolator};
    REQUIRE(deck.load(track));
    deck.set_playing(true);
    REQUIRE(render_left(deck, 500).back() == 0.0F);
    REQUIRE(deck.waiting());
    REQUIRE(deck.position() == 0.0);
}

TEST_CASE("Deck: rate 0 holds the position while playing", "[engine][deck]") {
    const Interpolator interpolator;
    const std::vector<float> samples = ramp_samples(10'000);
    Track track{samples, kRate};
    track.publish(10'000, true);
    Deck deck{interpolator};
    REQUIRE(deck.load(track));
    deck.set_rate(0.0);
    deck.seek(100.0);
    deck.set_playing(true);
    const std::vector<float> out = render_left(deck, 500);
    REQUIRE(deck.position() == 100.0);
    REQUIRE(deck.playing());
    REQUIRE(out.back() == 101.0F); // the held frame, at full gain
}

TEST_CASE("Deck: seeks and cues are clamped to the track", "[engine][deck]") {
    const Interpolator interpolator;
    const std::vector<float> samples = ramp_samples(1'000);
    Track track{samples, kRate};
    track.publish(1'000, true);
    Deck deck{interpolator};
    REQUIRE(deck.load(track));
    deck.seek(-50.0);
    REQUIRE(deck.position() == 0.0);
    deck.set_cue(1e12);
    REQUIRE(deck.cue() == 1'000.0);
    deck.set_cue(std::nan(""));
    REQUIRE(deck.cue() == 1'000.0);
    deck.seek(250.25);
    REQUIRE(deck.position() == 250.25);
}
