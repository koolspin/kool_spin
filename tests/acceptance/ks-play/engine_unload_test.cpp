// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/ks-play/spec.md: ks-engine unloading decks, with
// the test as its supervisor and as the worker that writes the segments
// (requirements 3 and 4).

#include <ks/ipc/log.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/track_segment.h>

#include "unload_support.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace {

using ks::ipc::LogLevel;
using ks::ipc::TrackStatus;
using ks::ks_play::test::has;
using ks::ks_play::test::has_any;
using ks::ks_play::test::send_unload;
using ks::track_loading::test::attach_ring;
using ks::track_loading::test::deck_shows;
using ks::track_loading::test::DeckView;
using ks::track_loading::test::keeps_beating;
using ks::track_loading::test::load_track;
using ks::track_loading::test::position_advances;
using ks::track_loading::test::receive_until;
using ks::track_loading::test::Received;
using ks::track_loading::test::set_playing;
using ks::track_loading::test::start_engine;
using ks::track_loading::test::TestEngine;
using ks::track_loading::test::TestTrack;
using ks::track_loading::test::track_loaded;
namespace msg = ks::ipc::msg;

constexpr std::uint32_t kRate = 44'100;

/// A track segment in `engine`'s runtime directory with `frames` frames
/// published, complete.
std::unique_ptr<TestTrack> new_track(const TestEngine& engine, std::uint64_t frames) {
    std::unique_ptr<TestTrack> track = TestTrack::create(engine.factory());
    track->start(kRate);
    track->finish(TrackStatus::Complete, frames);
    return track;
}

/// True if deck `deck` shows no track, and isn't playing.
bool shows_empty(const TestEngine& engine, std::size_t deck) {
    DeckView last;
    const bool shown = deck_shows(
        *engine.decks, deck,
        [](const DeckView& view) { return view.track_id == 0 && !view.loaded && !view.playing; },
        last);
    CAPTURE(deck, last.track_id, last.loaded, last.playing);
    return shown;
}

/// Sends an UnloadDeck for `deck`, and returns what arrives until its answer
/// does, plus, if `release` is true, a TrackReleased.
std::vector<Received> unload(TestEngine& engine, std::uint32_t deck, bool release = false) {
    const std::uint32_t id = engine.next_request++;
    REQUIRE(send_unload(engine.socket(), id, deck));
    return receive_until(engine.socket(), [id, release](const std::vector<Received>& so_far) {
        return has<msg::DeckUnloaded>(so_far, id) &&
               (!release || has_any<msg::TrackReleased>(so_far));
    });
}

} // namespace

TEST_CASE("Engine unload: a loaded deck, an empty deck", "[engine][unload]") {
    const std::unique_ptr<TestEngine> engine = start_engine();

    const std::unique_ptr<TestTrack> track = new_track(*engine, 44'100);
    REQUIRE(track_loaded(load_track(*engine, track->fd(), 7, 0, kRate)));
    DeckView last;
    REQUIRE(deck_shows(
        *engine->decks, 0, [](const DeckView& view) { return view.track_id == 7; }, last));

    // The answer, and then the release of the track the deck held.
    const std::uint32_t id = engine->next_request;
    const std::vector<Received> received = unload(*engine, 0, true);
    CHECK(has<msg::DeckUnloaded>(received, id));
    const auto released = ks::ks_play::test::first_of<msg::TrackReleased>(received);
    REQUIRE(released.has_value());
    CHECK(released.value_or(msg::TrackReleased{}).track_id == 7);
    CHECK(shows_empty(*engine, 0));

    // A deck with no track, and one that never had one.
    CHECK(has<msg::DeckUnloaded>(unload(*engine, 0), engine->next_request - 1));
    CHECK(has<msg::DeckUnloaded>(unload(*engine, 1), engine->next_request - 1));

    CAPTURE(engine->log->all());
    CHECK(engine->log->has(LogLevel::Info, "unloaded deck 1"));
    CHECK(engine->log->has(LogLevel::Info, "unloaded deck 2"));
    CHECK(keeps_beating(*engine->decks));
}

TEST_CASE("Engine unload: while playing", "[engine][unload]") {
    const std::unique_ptr<TestEngine> engine = start_engine();

    const std::unique_ptr<TestTrack> first = new_track(*engine, 441'000);
    const std::unique_ptr<TestTrack> second = new_track(*engine, 441'000);
    REQUIRE(track_loaded(load_track(*engine, first->fd(), 1, 0, kRate)));
    REQUIRE(track_loaded(load_track(*engine, second->fd(), 2, 1, kRate)));
    ks::client_rings::test::ClientRing ring = attach_ring(*engine);
    REQUIRE(ring.push(set_playing(0, true, 1)));
    REQUIRE(ring.push(set_playing(1, true, 2)));
    REQUIRE(position_advances(*engine->decks, 0));
    REQUIRE(position_advances(*engine->decks, 1));

    const std::uint32_t id = engine->next_request;
    CHECK(has<msg::DeckUnloaded>(unload(*engine, 0), id));
    CHECK(shows_empty(*engine, 0));
    CHECK(position_advances(*engine->decks, 1));
}
