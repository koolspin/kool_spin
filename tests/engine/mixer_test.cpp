// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/mixer.h>

#include <ks/engine/interpolator.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <limits>

namespace {

using ks::engine::BusMap;
using ks::engine::CrossfaderSide;
using ks::engine::Interpolator;
using ks::engine::kDecks;
using ks::engine::kMaxTrimDb;
using ks::engine::kMinCrossfaderTransition;
using ks::engine::Mixer;
using ks::engine::MixerSettings;

constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

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
