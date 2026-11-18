// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/track-loading/spec.md: ks-engine loading,
// playing, and releasing track segments, with the test as its supervisor and
// as the worker that writes the segments (requirements 3 to 8).

#include <ks/ipc/command.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/layout.h>
#include <ks/ipc/log.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/segment.h>
#include <ks/ipc/state.h>
#include <ks/ipc/track_segment.h>

#include "tracks_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

namespace {

using ks::ipc::ErrorCode;
using ks::ipc::kTrackLayoutVersion;
using ks::ipc::kTrackSamplesOffset;
using ks::ipc::kTrackSegmentSize;
using ks::ipc::LogLevel;
using ks::ipc::SegmentKind;
using ks::ipc::TrackStatus;
using ks::track_loading::test::attach_ring;
using ks::track_loading::test::deck_shows;
using ks::track_loading::test::DeckView;
using ks::track_loading::test::is_error;
using ks::track_loading::test::keeps_beating;
using ks::track_loading::test::kReach;
using ks::track_loading::test::load_track;
using ks::track_loading::test::Milliseconds;
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

/// A new track segment in `engine`'s runtime directory, with its rate
/// stored.
std::unique_ptr<TestTrack> new_track(const TestEngine& engine, std::uint32_t rate = kRate) {
    std::unique_ptr<TestTrack> track = TestTrack::create(engine.factory());
    track->start(rate);
    return track;
}

/// True if deck `deck` shows `frames` frames ready of track `id`, complete
/// or not as `complete` says.
bool shows_frames(const TestEngine& engine, std::size_t deck, std::uint64_t id,
                  std::uint64_t frames, bool complete) {
    DeckView last;
    const bool shown = deck_shows(
        *engine.decks, deck,
        [&](const DeckView& view) {
            return view.track_id == id && view.frames_ready == frames && view.complete == complete;
        },
        last);
    CAPTURE(deck, id, frames, complete, last.track_id, last.frames_ready, last.complete);
    return shown;
}

} // namespace

TEST_CASE("Engine tracks: load, play, and release", "[engine][tracks]") {
    const std::unique_ptr<TestEngine> engine = start_engine();

    const std::unique_ptr<TestTrack> first = new_track(*engine);
    first->publish(44'100);
    REQUIRE(track_loaded(load_track(*engine, first->fd(), 7, 0, kRate)));

    DeckView last;
    const bool shown = deck_shows(
        *engine->decks, 0,
        [](const DeckView& view) {
            return view.track_id == 7 && view.sample_rate == kRate && view.frames_ready == 44'100 &&
                   !view.complete && view.loaded;
        },
        last);
    CAPTURE(last.track_id, last.sample_rate, last.frames_ready, last.complete);
    CHECK(shown);

    first->finish(TrackStatus::Complete, 88'200);
    CHECK(shows_frames(*engine, 0, 7, 88'200, true));

    ks::client_rings::test::ClientRing ring = attach_ring(*engine);
    REQUIRE(ring.push(set_playing(0, true, 1)));
    CHECK(position_advances(*engine->decks, 0));

    const std::unique_ptr<TestTrack> second = new_track(*engine);
    second->publish(1'000);
    REQUIRE(track_loaded(load_track(*engine, second->fd(), 8, 0, kRate)));

    // Track 7's release is a notification: request ID 0.
    const std::vector<Received> received =
        receive_until(engine->socket(), [](const std::vector<Received>& so_far) {
            return std::ranges::any_of(so_far, [](const Received& r) {
                return std::holds_alternative<msg::TrackReleased>(r.message.body);
            });
        });
    const auto released = std::ranges::find_if(received, [](const Received& r) {
        return std::holds_alternative<msg::TrackReleased>(r.message.body);
    });
    REQUIRE(released != received.end());
    CHECK(released->message.request_id == 0);
    CHECK(std::get<msg::TrackReleased>(released->message.body).track_id == 7);

#ifdef __linux__
    // The engine has unmapped track 7's segment by the time it says so.
    const ks::track_loading::test::FileKey key = ks::track_loading::test::key_of(first->fd().get());
    CHECK_FALSE(ks::track_loading::test::maps_file(engine->process().pid(), key));
    // Track 8's is still mapped.
    CHECK(ks::track_loading::test::maps_file(engine->process().pid(),
                                             ks::track_loading::test::key_of(second->fd().get())));
#endif

    CAPTURE(engine->log->all());
    CHECK(engine->log->has(LogLevel::Info, "loaded track 7 on deck 1"));
    CHECK(engine->log->has(LogLevel::Info, "released track 7"));
}

TEST_CASE("Engine tracks: playing while decoding", "[engine][tracks]") {
    const std::unique_ptr<TestEngine> engine = start_engine();

    const std::unique_ptr<TestTrack> track = new_track(*engine);
    REQUIRE(track_loaded(load_track(*engine, track->fd(), 1, 0, kRate)));
    CHECK(shows_frames(*engine, 0, 1, 0, false));

    ks::client_rings::test::ClientRing ring = attach_ring(*engine);
    REQUIRE(ring.push(set_playing(0, true, 1)));
    track->publish(4'410);

    DeckView last;
    const bool waits = deck_shows(
        *engine->decks, 0,
        [](const DeckView& view) {
            return view.frames_ready == 4'410 && view.waiting && view.position == 4'410.0 - kReach;
        },
        last);
    CAPTURE(last.frames_ready, last.waiting, last.position);
    CHECK(waits);

    track->finish(TrackStatus::Complete, 44'100);
    const bool ends = deck_shows(
        *engine->decks, 0,
        [](const DeckView& view) {
            return view.complete && view.frames_ready == 44'100 && view.position == 44'100.0 &&
                   !view.waiting && !view.playing;
        },
        last, Milliseconds{10'000});
    CAPTURE(last.complete, last.frames_ready, last.position, last.waiting, last.playing);
    CHECK(ends);
}

#ifdef __linux__
TEST_CASE("Engine tracks: published pages are faulted in", "[engine][tracks]") {
    const std::unique_ptr<TestEngine> engine = start_engine();

    const std::unique_ptr<TestTrack> track = new_track(*engine);
    REQUIRE(track_loaded(load_track(*engine, track->fd(), 1, 0, kRate)));
    CHECK(shows_frames(*engine, 0, 1, 0, false));

    // 10 s at 44.1 kHz: 3,528,000 bytes. The deck stays paused, so only the
    // control thread touches them.
    track->publish(441'000);
    const ks::track_loading::test::FileKey key = ks::track_loading::test::key_of(track->fd().get());
    std::optional<std::uint64_t> rss;
    const bool faulted_in = ks::track_loading::test::eventually(
        [&] {
            rss = ks::track_loading::test::rss_kb(engine->process().pid(), key);
            return rss.value_or(0) >= 3'445;
        },
        Milliseconds{1'000});
    CAPTURE(rss.value_or(0));
    CHECK(faulted_in);
}
#endif

TEST_CASE("Engine tracks: what it refuses", "[engine][tracks]") {
    const std::unique_ptr<TestEngine> engine = start_engine();

    const std::unique_ptr<TestTrack> held = new_track(*engine);
    REQUIRE(track_loaded(load_track(*engine, held->fd(), 5, 0, kRate)));

    SECTION("an engine-state segment") {
        const ks::ipc::Fd state = ks::client_rings::test::create_segment(
            engine->supervised(), SegmentKind::EngineState, ks::ipc::kEngineStateLayoutVersion,
            ks::ipc::kEngineStateSegmentSize);
        CHECK(is_error(load_track(*engine, state, 9, 1, kRate), ErrorCode::Unavailable));
    }
    SECTION("a track segment at layout version 2") {
        const std::unique_ptr<TestTrack> track =
            TestTrack::create(engine->factory(), kTrackLayoutVersion + 1, kTrackSegmentSize);
        CHECK(is_error(load_track(*engine, track->fd(), 9, 1, kRate), ErrorCode::Unavailable));
    }
    SECTION("a track segment that holds no frames") {
        const std::unique_ptr<TestTrack> track =
            TestTrack::create(engine->factory(), kTrackLayoutVersion, kTrackSamplesOffset);
        CHECK(is_error(load_track(*engine, track->fd(), 9, 1, kRate), ErrorCode::Unavailable));
    }
    SECTION("the ID of a track it holds") {
        const std::unique_ptr<TestTrack> track = new_track(*engine);
        CHECK(is_error(load_track(*engine, track->fd(), 5, 1, kRate), ErrorCode::Unavailable));
    }

    CHECK(keeps_beating(*engine->decks));
    const std::unique_ptr<TestTrack> valid = new_track(*engine);
    CHECK(track_loaded(load_track(*engine, valid->fd(), 6, 1, kRate)));
}

TEST_CASE("Engine tracks: a corrupt header", "[engine][tracks]") {
    const std::unique_ptr<TestEngine> engine = start_engine();

    const std::unique_ptr<TestTrack> bad_status = new_track(*engine);
    const std::unique_ptr<TestTrack> going_down = new_track(*engine);
    const std::unique_ptr<TestTrack> wrong_rate = new_track(*engine, 48'000);
    REQUIRE(track_loaded(load_track(*engine, bad_status->fd(), 1, 0, kRate)));
    REQUIRE(track_loaded(load_track(*engine, going_down->fd(), 2, 1, kRate)));
    REQUIRE(track_loaded(load_track(*engine, wrong_rate->fd(), 3, 2, kRate)));

    // A status that isn't a TrackStatus.
    bad_status->publish(1'000);
    REQUIRE(shows_frames(*engine, 0, 1, 1'000, false));
    ks::ipc::store_shared(bad_status->header().status, 7, std::memory_order_relaxed);
    CHECK(shows_frames(*engine, 0, 1, 1'000, true));

    // Frames that go down.
    going_down->publish(2'000);
    REQUIRE(shows_frames(*engine, 1, 2, 2'000, false));
    ks::ipc::store_shared(going_down->header().ready, 1'000, std::memory_order_release);
    CHECK(shows_frames(*engine, 1, 2, 2'000, true));

    // A rate that isn't LoadTrack's.
    wrong_rate->publish(3'000);
    CHECK(shows_frames(*engine, 2, 3, 0, true));

    // Nothing the header says afterwards changes them.
    bad_status->publish(10'000);
    ks::ipc::store_shared(going_down->header().ready, 10'000, std::memory_order_release);
    wrong_rate->publish(10'000);
    std::this_thread::sleep_for(Milliseconds{500});
    CHECK(shows_frames(*engine, 0, 1, 1'000, true));
    CHECK(shows_frames(*engine, 1, 2, 2'000, true));
    CHECK(shows_frames(*engine, 2, 3, 0, true));

    CHECK(keeps_beating(*engine->decks));
    CAPTURE(engine->log->all());
    CHECK(engine->log->has(LogLevel::Warning, "track 1's header is corrupt"));
    CHECK(engine->log->has(LogLevel::Warning, "track 2's header is corrupt"));
    CHECK(engine->log->has(LogLevel::Warning, "track 3's header is corrupt"));
}

TEST_CASE("Engine tracks: progress without the supervisor", "[engine][tracks]") {
    const std::unique_ptr<TestEngine> engine = start_engine();

    const std::unique_ptr<TestTrack> track = new_track(*engine);
    REQUIRE(track_loaded(load_track(*engine, track->fd(), 1, 0, kRate)));
    track->publish(1'000);
    REQUIRE(shows_frames(*engine, 0, 1, 1'000, false));

    engine->supervised().socket.reset();
    track->finish(TrackStatus::Complete, 44'100);
    CHECK(shows_frames(*engine, 0, 1, 44'100, true));

    engine->process().signal(SIGTERM);
    CHECK(engine->process().wait() == 0);
}
