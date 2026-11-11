// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/deck/spec.md: real-time safety.

#include <ks/core/realtime.h>

#include <ks/engine/deck.h>
#include <ks/engine/interpolator.h>
#include <ks/engine/track.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace {

using ks::engine::Deck;
using ks::engine::Interpolator;
using ks::engine::Track;
using ks::engine::test::kRate;
using ks::engine::test::kSignal;
using ks::engine::test::OwnedTrack;

constexpr std::size_t kFrames = 64;

struct Results {
    bool loaded = false;
    bool held = false;
    bool playing = false;
    bool waiting = true;
    double rate = 0.0;
    double position = 0.0;
    double cue = 0.0;
    bool unloaded = false;
};

// Every call, and the interpolator inside render(), is checked by function
// effect analysis at compile time, and by RealtimeSanitizer at run time under
// the rtsan preset.
Results play_a_block(Deck& deck, const Track& track, std::span<float> left,
                     std::span<float> right) noexcept KS_NONBLOCKING {
    Results results;
    static_cast<void>(track.ready());
    results.loaded = deck.load(track);
    deck.set_rate(1.5);
    deck.seek(1'000.0);
    deck.set_cue(500.0);
    deck.set_playing(true);
    deck.render(left, right, kRate);
    results.held = deck.holds(track);
    results.playing = deck.playing();
    results.waiting = deck.waiting();
    results.rate = deck.rate();
    results.position = deck.position();
    results.cue = deck.cue();
    deck.unload();
    deck.render(left, right, kRate);
    results.unloaded = deck.track() == nullptr;
    return results;
}

} // namespace

TEST_CASE("Deck is real-time safe", "[deck][realtime]") {
    const Interpolator interpolator;
    OwnedTrack owned{10'000, kRate, kSignal};
    Deck deck{interpolator};
    std::vector<float> left(kFrames);
    std::vector<float> right(kFrames);

    const Results results = play_a_block(deck, owned.track(), left, right);
    REQUIRE(results.loaded);
    REQUIRE(results.held);
    REQUIRE(results.playing);
    REQUIRE_FALSE(results.waiting);
    REQUIRE(results.rate == 1.5);
    REQUIRE(results.position > 1'000.0);
    REQUIRE(results.cue == 500.0);
    REQUIRE(results.unloaded);
}
