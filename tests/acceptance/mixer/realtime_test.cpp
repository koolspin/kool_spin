// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/mixer/spec.md: real-time safety.

#include <ks/core/realtime.h>

#include <ks/engine/block.h>
#include <ks/engine/mixer.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace {

using ks::engine::Block;
using ks::engine::BusMap;
using ks::engine::CrossfaderSide;
using ks::engine::Mixer;
using ks::engine::test::kRate;
using ks::engine::test::Rig;

constexpr std::size_t kFrames = 64;

// Every call, and the decks and gains inside process(), is checked by
// function effect analysis at compile time, and by RealtimeSanitizer at run
// time under the rtsan preset.
double mix_a_block(Mixer& mixer,
                   std::span<const std::span<float>> channels) noexcept KS_NONBLOCKING {
    mixer.deck(0).set_playing(true);
    mixer.set_trim(0, 3.0);
    mixer.set_fader(0, 0.8);
    mixer.set_crossfader_assign(0, CrossfaderSide::A);
    mixer.set_cue(0, true);
    mixer.set_crossfader(0.25);
    mixer.set_crossfader_curve(0.5);
    mixer.set_main_level(-1.0);
    mixer.set_headphone_level(-2.0);
    mixer.set_cue_mix(0.25);
    mixer.set_bus_map(BusMap{.main = 0, .cue = 2});
    const Block block{.channels = channels,
                      .frames = kFrames,
                      .sample_rate = kRate,
                      .host_time = {},
                      .resumed = false};
    mixer.process(block);
    return mixer.settings().crossfader;
}

} // namespace

TEST_CASE("Mixer is real-time safe", "[mixer][realtime]") {
    Rig rig;
    std::array<std::vector<float>, 4> storage{
        std::vector<float>(kFrames), std::vector<float>(kFrames), std::vector<float>(kFrames),
        std::vector<float>(kFrames)};
    const std::array<std::span<float>, 4> channels{std::span{storage[0]}, std::span{storage[1]},
                                                   std::span{storage[2]}, std::span{storage[3]}};

    REQUIRE(mix_a_block(rig.mixer, channels) == 0.25);
    // Deck 0 started playing: its ramp has begun on the main bus.
    REQUIRE(storage[0].back() > 0.0F);
}
