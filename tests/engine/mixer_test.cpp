// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/mixer.h>

#include <ks/engine/block.h>
#include <ks/engine/interpolator.h>
#include <ks/engine/track.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace {

using ks::engine::Block;
using ks::engine::BusMap;
using ks::engine::CrossfaderSide;
using ks::engine::Interpolator;
using ks::engine::kDecks;
using ks::engine::kMaxBlockFrames;
using ks::engine::kMaxTrimDb;
using ks::engine::kMinCrossfaderTransition;
using ks::engine::Mixer;
using ks::engine::MixerSettings;
using ks::engine::Track;

constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::uint32_t kRate = 48'000;
constexpr std::size_t kTrackFrames = 10'000;
/// Enough frames for the decks' play ramps to finish.
constexpr std::size_t kSettle = 2'000;
/// What the output is filled with before rendering, to show what's written.
constexpr float kSentinel = 9.0F;

/// Deck i's constant on the left; the right is its negative.
float deck_value(std::size_t deck) {
    return 0.05F * static_cast<float>(deck + 1);
}

/// A mixer whose decks are loaded with constant tracks.
struct Rig {
    explicit Rig(const BusMap& map = {}) : mixer{interpolator, map} {
        samples.reserve(kDecks);
        for (std::size_t i = 0; i < kDecks; ++i) {
            auto& data = samples.emplace_back(kTrackFrames * 2);
            for (std::size_t frame = 0; frame < kTrackFrames; ++frame) {
                data.at(frame * 2) = deck_value(i);
                data.at((frame * 2) + 1) = -deck_value(i);
            }
            const auto& track = tracks.emplace_back(std::make_unique<Track>(data, kRate));
            track->publish(kTrackFrames, true);
            REQUIRE(mixer.deck(i).load(*track));
        }
    }

    void play_all() {
        for (std::size_t i = 0; i < kDecks; ++i) {
            mixer.deck(i).set_playing(true);
        }
    }

    Interpolator interpolator;
    std::vector<std::vector<float>> samples;
    std::vector<std::unique_ptr<Track>> tracks;
    Mixer mixer;
};

using Outputs = std::vector<std::vector<float>>;

/// Renders `frames` frames of `channels` channels, prefilled with
/// kSentinel, in blocks of 64.
Outputs render(Mixer& mixer, std::size_t frames, std::size_t channels) {
    Outputs out(channels, std::vector<float>(frames, kSentinel));
    std::vector<std::span<float>> views(channels);
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min<std::size_t>(64, frames - done);
        for (std::size_t c = 0; c < channels; ++c) {
            views.at(c) = std::span{out.at(c)}.subspan(done, n);
        }
        mixer.process(Block{.channels = views,
                            .frames = n,
                            .sample_rate = kRate,
                            .host_time = {},
                            .resumed = false});
        done += n;
    }
    return out;
}

bool all_equal(const std::vector<float>& channel, float value) {
    return std::ranges::all_of(channel, [value](float sample) { return sample == value; });
}

bool near(float actual, double expected) {
    return std::abs(static_cast<double>(actual) - expected) <= 1e-6;
}

TEST_CASE("Mixer: an 8-channel block with map {4, 6} writes only channels 4 to 7", "[mixer]") {
    Rig rig{BusMap{.main = 4, .cue = 6}};
    rig.play_all();
    const Outputs out = render(rig.mixer, kSettle, 8);
    for (std::size_t c = 0; c < 4; ++c) {
        CAPTURE(c);
        REQUIRE(all_equal(out.at(c), kSentinel));
    }
    REQUIRE(near(out.at(4).back(), 0.5));
    REQUIRE(near(out.at(5).back(), -0.5));
    // Nothing is cued, and the cue mix is 0: the headphones are silent.
    REQUIRE(all_equal(out.at(6), 0.0F));
    REQUIRE(all_equal(out.at(7), 0.0F));
}

TEST_CASE("Mixer: a 3-channel block writes only the main bus", "[mixer]") {
    Rig rig;
    rig.play_all();
    const Outputs out = render(rig.mixer, kSettle, 3);
    REQUIRE(near(out.at(0).back(), 0.5));
    REQUIRE(near(out.at(1).back(), -0.5));
    REQUIRE(all_equal(out.at(2), kSentinel));
}

TEST_CASE("Mixer: an invalid block writes nothing and renders nothing", "[mixer]") {
    Rig rig;
    rig.play_all();
    std::vector<std::vector<float>> out(4, std::vector<float>(kMaxBlockFrames + 1, kSentinel));
    std::vector<std::span<float>> views;
    views.reserve(out.size());
    for (auto& channel : out) {
        views.emplace_back(channel);
    }
    const auto block = [&views](std::size_t frames, std::uint32_t sample_rate) {
        return Block{.channels = views,
                     .frames = frames,
                     .sample_rate = sample_rate,
                     .host_time = {},
                     .resumed = false};
    };

    rig.mixer.process(block(0, kRate));
    rig.mixer.process(block(kMaxBlockFrames + 1, kRate));
    rig.mixer.process(block(64, 0));
    // The cue bus's right channel is shorter than the block.
    views.at(3) = views.at(3).first(10);
    rig.mixer.process(block(64, kRate));

    for (const auto& channel : out) {
        REQUIRE(all_equal(channel, kSentinel));
    }
    for (std::size_t i = 0; i < kDecks; ++i) {
        REQUIRE(rig.mixer.deck(i).position() == 0.0);
    }
}

TEST_CASE("Mixer: paused decks give silence", "[mixer]") {
    Rig rig;
    const Outputs out = render(rig.mixer, 256, 4);
    for (const auto& channel : out) {
        REQUIRE(all_equal(channel, 0.0F));
    }
}

TEST_CASE("Mixer: a main level of -inf silences main but not the headphones", "[mixer]") {
    Rig rig;
    rig.play_all();
    rig.mixer.set_main_level(-kInfinity);
    rig.mixer.set_cue(0, true);

    SECTION("cue mix 0: the headphones carry the cued deck") {
        const Outputs out = render(rig.mixer, kSettle, 4);
        REQUIRE(all_equal(out.at(0), 0.0F));
        REQUIRE(all_equal(out.at(1), 0.0F));
        REQUIRE(near(out.at(2).back(), static_cast<double>(deck_value(0))));
        REQUIRE(near(out.at(3).back(), -static_cast<double>(deck_value(0))));
    }
    SECTION("cue mix 1: the headphones carry the main bus before the main level") {
        rig.mixer.set_cue_mix(1.0);
        const Outputs out = render(rig.mixer, kSettle, 4);
        REQUIRE(all_equal(out.at(0), 0.0F));
        REQUIRE(near(out.at(2).back(), 0.5));
        REQUIRE(near(out.at(3).back(), -0.5));
    }
}

TEST_CASE("Mixer: the defaults are MixerSettings{}", "[mixer]") {
    const Interpolator interpolator;
    const Mixer mixer{interpolator};
    REQUIRE(mixer.settings() == MixerSettings{});
}

TEST_CASE("Mixer: per-channel controls clamp at and past their bounds", "[mixer]") {
    const Interpolator interpolator;
    Mixer mixer{interpolator};
    const auto& channel = mixer.settings().channels.at(1);

    mixer.set_trim(1, kMaxTrimDb);
    REQUIRE(channel.trim_db == kMaxTrimDb);
    mixer.set_trim(1, 12.5);
    REQUIRE(channel.trim_db == kMaxTrimDb);
    mixer.set_trim(1, kInfinity);
    REQUIRE(channel.trim_db == kMaxTrimDb);
    mixer.set_trim(1, -kInfinity);
    REQUIRE(channel.trim_db == -kInfinity);
    mixer.set_trim(1, -60.0);
    REQUIRE(channel.trim_db == -60.0);

    mixer.set_fader(1, 0.0);
    REQUIRE(channel.fader == 0.0);
    mixer.set_fader(1, 1.0);
    REQUIRE(channel.fader == 1.0);
    mixer.set_fader(1, 1.5);
    REQUIRE(channel.fader == 1.0);
    mixer.set_fader(1, kInfinity);
    REQUIRE(channel.fader == 1.0);
    mixer.set_fader(1, -0.5);
    REQUIRE(channel.fader == 0.0);
    mixer.set_fader(1, -kInfinity);
    REQUIRE(channel.fader == 0.0);
    mixer.set_fader(1, 0.3);
    REQUIRE(channel.fader == 0.3);

    mixer.set_crossfader_assign(1, CrossfaderSide::A);
    REQUIRE(channel.side == CrossfaderSide::A);
    mixer.set_crossfader_assign(1, CrossfaderSide::B);
    REQUIRE(channel.side == CrossfaderSide::B);
    mixer.set_crossfader_assign(1, CrossfaderSide::Thru);
    REQUIRE(channel.side == CrossfaderSide::Thru);

    mixer.set_cue(1, true);
    REQUIRE(channel.cue);
    mixer.set_cue(1, false);
    REQUIRE_FALSE(channel.cue);

    // Only deck 1 changed.
    REQUIRE(mixer.settings().channels.at(0) == ks::engine::ChannelSettings{});
    REQUIRE(mixer.settings().channels.at(2) == ks::engine::ChannelSettings{});
    REQUIRE(mixer.settings().channels.at(3) == ks::engine::ChannelSettings{});
}

TEST_CASE("Mixer: global controls clamp at and past their bounds", "[mixer]") {
    const Interpolator interpolator;
    Mixer mixer{interpolator};
    const MixerSettings& settings = mixer.settings();

    mixer.set_crossfader(0.0);
    REQUIRE(settings.crossfader == 0.0);
    mixer.set_crossfader(1.0);
    REQUIRE(settings.crossfader == 1.0);
    mixer.set_crossfader(kInfinity);
    REQUIRE(settings.crossfader == 1.0);
    mixer.set_crossfader(-kInfinity);
    REQUIRE(settings.crossfader == 0.0);
    mixer.set_crossfader(0.75);
    REQUIRE(settings.crossfader == 0.75);

    mixer.set_crossfader_curve(kMinCrossfaderTransition);
    REQUIRE(settings.crossfader_transition == kMinCrossfaderTransition);
    mixer.set_crossfader_curve(0.005);
    REQUIRE(settings.crossfader_transition == kMinCrossfaderTransition);
    mixer.set_crossfader_curve(-kInfinity);
    REQUIRE(settings.crossfader_transition == kMinCrossfaderTransition);
    mixer.set_crossfader_curve(kInfinity);
    REQUIRE(settings.crossfader_transition == 1.0);
    mixer.set_crossfader_curve(0.2);
    REQUIRE(settings.crossfader_transition == 0.2);

    mixer.set_main_level(0.0);
    REQUIRE(settings.main_level_db == 0.0);
    mixer.set_main_level(kInfinity);
    REQUIRE(settings.main_level_db == 0.0);
    mixer.set_main_level(-kInfinity);
    REQUIRE(settings.main_level_db == -kInfinity);
    mixer.set_main_level(-6.0);
    REQUIRE(settings.main_level_db == -6.0);

    mixer.set_headphone_level(0.1);
    REQUIRE(settings.headphone_level_db == 0.0);
    mixer.set_headphone_level(kInfinity);
    REQUIRE(settings.headphone_level_db == 0.0);
    mixer.set_headphone_level(-kInfinity);
    REQUIRE(settings.headphone_level_db == -kInfinity);
    mixer.set_headphone_level(-3.0);
    REQUIRE(settings.headphone_level_db == -3.0);

    mixer.set_cue_mix(1.0);
    REQUIRE(settings.cue_mix == 1.0);
    mixer.set_cue_mix(kInfinity);
    REQUIRE(settings.cue_mix == 1.0);
    mixer.set_cue_mix(-kInfinity);
    REQUIRE(settings.cue_mix == 0.0);
    mixer.set_cue_mix(0.5);
    REQUIRE(settings.cue_mix == 0.5);
}

TEST_CASE("Mixer: NaN and out-of-range deck indices change nothing", "[mixer]") {
    const Interpolator interpolator;
    Mixer mixer{interpolator};
    mixer.set_trim(0, -3.0);
    mixer.set_fader(0, 0.5);
    mixer.set_crossfader(0.25);
    mixer.set_crossfader_curve(0.5);
    mixer.set_main_level(-1.0);
    mixer.set_headphone_level(-2.0);
    mixer.set_cue_mix(0.75);
    const MixerSettings before = mixer.settings();

    for (std::size_t deck = 0; deck < kDecks; ++deck) {
        mixer.set_trim(deck, kNaN);
        mixer.set_fader(deck, kNaN);
    }
    mixer.set_crossfader(kNaN);
    mixer.set_crossfader_curve(kNaN);
    mixer.set_main_level(kNaN);
    mixer.set_headphone_level(kNaN);
    mixer.set_cue_mix(kNaN);
    for (const std::size_t deck : {kDecks, kDecks + 1, std::numeric_limits<std::size_t>::max()}) {
        mixer.set_trim(deck, 3.0);
        mixer.set_fader(deck, 0.1);
        mixer.set_crossfader_assign(deck, CrossfaderSide::A);
        mixer.set_cue(deck, true);
    }
    REQUIRE(mixer.settings() == before);
}

TEST_CASE("Mixer: valid bus maps are accepted", "[mixer]") {
    const Interpolator interpolator;
    for (const BusMap map :
         {BusMap{.main = 0, .cue = 2}, BusMap{.main = 2, .cue = 0}, BusMap{.main = 4, .cue = 6},
          BusMap{.main = 6, .cue = 4}, BusMap{.main = 6, .cue = 0}}) {
        const Mixer constructed{interpolator, map};
        REQUIRE(constructed.settings().bus_map == map);

        Mixer mixer{interpolator};
        mixer.set_bus_map(map);
        REQUIRE(mixer.settings().bus_map == map);
    }
}

TEST_CASE("Mixer: invalid bus maps are refused", "[mixer]") {
    const Interpolator interpolator;
    const BusMap previous{.main = 4, .cue = 6};
    for (const BusMap map : {
             BusMap{.main = 1, .cue = 2}, // odd main
             BusMap{.main = 0, .cue = 3}, // odd cue
             BusMap{.main = 2, .cue = 2}, // equal
             BusMap{.main = 8, .cue = 2}, // main out of range
             BusMap{.main = 0, .cue = 8}, // cue out of range
             BusMap{.main = 7, .cue = 0}, // odd, and its pair is out of range
             BusMap{.main = 0, .cue = std::numeric_limits<std::size_t>::max() - 1},
         }) {
        const Mixer constructed{interpolator, map};
        REQUIRE(constructed.settings().bus_map == BusMap{});

        Mixer mixer{interpolator, previous};
        mixer.set_bus_map(map);
        REQUIRE(mixer.settings().bus_map == previous);
    }
}

} // namespace
