// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/resume.h>

#include <ks/engine/mixer.h>
#include <ks/ipc/crc32.h>
#include <ks/ipc/state.h>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace {

using ks::engine::CrossfaderSide;
using ks::engine::encode;
using ks::engine::Snapshot;

std::uint64_t bits(double value) {
    return std::bit_cast<std::uint64_t>(value);
}

TEST_CASE("Resume encoding: one snapshot's words", "[resume]") {
    Snapshot snapshot;
    snapshot.generation = 7;
    snapshot.host_time =
        std::chrono::steady_clock::time_point{std::chrono::nanoseconds{123'456'789}};
    snapshot.decks[0] = {
        .track_id = 11, .playing = true, .position = 100.5, .rate = 1.25, .cue = 3.0};
    snapshot.decks[1] = {
        .track_id = 0, .playing = false, .position = 0.0, .rate = -0.5, .cue = 0.0};
    snapshot.decks[2] = {
        .track_id = 13, .playing = false, .position = -0.0, .rate = 1.0, .cue = 9.0};
    snapshot.mixer.channels[0] = {.trim_db = -std::numeric_limits<double>::infinity(),
                                  .fader = 0.25,
                                  .side = CrossfaderSide::A,
                                  .cue = true};
    snapshot.mixer.channels[1] = {
        .trim_db = 6.0, .fader = 0.5, .side = CrossfaderSide::B, .cue = false};
    snapshot.mixer.crossfader = 0.75;
    snapshot.mixer.crossfader_transition = 0.125;
    snapshot.mixer.main_level_db = -3.0;
    snapshot.mixer.headphone_level_db = -6.0;
    snapshot.mixer.cue_mix = 0.375;
    snapshot.mixer.bus_map = {.main = 4, .cue = 0};

    const ks::ipc::ResumeState words = encode(snapshot);

    CHECK(words.generation == 7);
    CHECK(words.host_time == 123'456'789);

    CHECK(words.decks[0].track_id == 11);
    CHECK(words.decks[0].flags == ks::ipc::kResumePlaying);
    CHECK(words.decks[0].position == bits(100.5));
    CHECK(words.decks[0].rate == bits(1.25));
    CHECK(words.decks[0].cue == bits(3.0));
    CHECK(words.decks[1].track_id == 0);
    CHECK(words.decks[1].flags == 0);
    CHECK(words.decks[1].rate == bits(-0.5));
    CHECK(words.decks[2].flags == 0);
    // −0.0 keeps its sign bit, so it round-trips.
    CHECK(words.decks[2].position == bits(-0.0));
    CHECK(words.decks[3].rate == bits(1.0));

    CHECK(words.channels[0].trim_db == bits(-std::numeric_limits<double>::infinity()));
    CHECK(words.channels[0].fader == bits(0.25));
    CHECK(words.channels[0].side == 0);
    CHECK(words.channels[0].cue == 1);
    CHECK(words.channels[1].trim_db == bits(6.0));
    CHECK(words.channels[1].side == 1);
    CHECK(words.channels[1].cue == 0);
    CHECK(words.channels[2].side == 2);
    CHECK(words.channels[2].fader == bits(1.0));

    CHECK(words.crossfader == bits(0.75));
    CHECK(words.crossfader_transition == bits(0.125));
    CHECK(words.main_level_db == bits(-3.0));
    CHECK(words.headphone_level_db == bits(-6.0));
    CHECK(words.cue_mix == bits(0.375));
    CHECK(words.bus_main == 4);
    CHECK(words.bus_cue == 0);

    const std::span<const std::byte> before_checksum =
        std::as_bytes(std::span{&words, 1}).first(offsetof(ks::ipc::ResumeState, checksum));
    CHECK(words.checksum == ks::ipc::crc32(before_checksum));
    CHECK((words.checksum >> 32U) == 0);
}

} // namespace
