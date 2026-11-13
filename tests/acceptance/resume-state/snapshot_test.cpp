// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/resume-state/spec.md: snapshots, their
// encoding and validation, and choosing a slot (requirements 6 to 10 and 19).

#include <ks/engine/block.h>
#include <ks/engine/deck.h>
#include <ks/engine/mixer.h>
#include <ks/engine/resume.h>
#include <ks/engine/track.h>
#include <ks/ipc/crc32.h>
#include <ks/ipc/state.h>

#include "snapshot_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using ks::engine::BusMap;
using ks::engine::CrossfaderSide;
using ks::engine::DeckSnapshot;
using ks::engine::kMaxGeneration;
using ks::engine::Snapshot;
using ks::engine::resume_test::at_ns;
using ks::engine::resume_test::bits;
using ks::engine::resume_test::expected_words;
using ks::engine::resume_test::kInfinity;
using ks::engine::resume_test::kNaN;
using ks::engine::resume_test::same_words;
using ks::engine::resume_test::with_checksum;
using ks::engine::resume_test::word_differences;
using ks::ipc::ResumeState;

constexpr auto kMaxFrames = static_cast<double>(ks::engine::kMaxTrackFrames);

/// A valid snapshot with a distinct value in every field: decks 0 to 2 hold
/// tracks, and deck 3 is empty.
Snapshot sample_snapshot(std::uint64_t generation) {
    Snapshot snapshot;
    snapshot.generation = generation;
    snapshot.host_time = at_ns(987'654'321'000);
    snapshot.decks = {{
        DeckSnapshot{.track_id = 11, .playing = true, .position = 1.5, .rate = 0.5, .cue = 10.5},
        DeckSnapshot{
            .track_id = 12, .playing = false, .position = 2.25, .rate = -1.25, .cue = 20.25},
        DeckSnapshot{
            .track_id = 13, .playing = true, .position = 3.125, .rate = 3.5, .cue = 30.125},
        DeckSnapshot{.track_id = 0, .playing = false, .position = 0.0, .rate = 0.75, .cue = 0.0},
    }};
    ks::engine::MixerSettings& mixer = snapshot.mixer;
    mixer.channels = {{
        {.trim_db = -1.5, .fader = 0.1, .side = CrossfaderSide::A, .cue = true},
        {.trim_db = 2.5, .fader = 0.2, .side = CrossfaderSide::B, .cue = false},
        {.trim_db = -3.5, .fader = 0.3, .side = CrossfaderSide::Thru, .cue = true},
        {.trim_db = 4.5, .fader = 0.4, .side = CrossfaderSide::A, .cue = false},
    }};
    mixer.crossfader = 0.6;
    mixer.crossfader_transition = 0.7;
    mixer.main_level_db = -5.5;
    mixer.headphone_level_db = -6.5;
    mixer.cue_mix = 0.8;
    mixer.bus_map = BusMap{.main = 4, .cue = 6};
    return snapshot;
}

/// Checks that `snapshot` validates back to itself, bit for bit, in its own
/// slot only, and encodes as requirement 3 says.
void require_round_trip(const Snapshot& snapshot) {
    const ResumeState words = ks::engine::encode(snapshot);
    INFO(word_differences(words, expected_words(snapshot)));
    REQUIRE(same_words(words, expected_words(snapshot)));
    const std::size_t slot = snapshot.generation % 2;
    const std::optional<Snapshot> back = ks::engine::validate(words, slot);
    REQUIRE(back.has_value());
    REQUIRE(back.value_or(Snapshot{}) == snapshot);
    REQUIRE(same_words(expected_words(back.value_or(Snapshot{})), words));
    REQUIRE_FALSE(ks::engine::validate(words, 1 - slot).has_value());
    REQUIRE_FALSE(ks::engine::validate(words, 2).has_value());
}

} // namespace

TEST_CASE("Resume state: encoding", "[resume][snapshot]") {
    const Snapshot snapshot = sample_snapshot(12'345);
    const ResumeState words = ks::engine::encode(snapshot);

    REQUIRE(words.generation == 12'345);
    REQUIRE(words.host_time == 987'654'321'000);
    REQUIRE(words.decks.at(0).track_id == 11);
    REQUIRE(words.decks.at(0).flags == ks::ipc::kResumePlaying);
    REQUIRE(words.decks.at(0).position == bits(1.5));
    REQUIRE(words.decks.at(0).rate == bits(0.5));
    REQUIRE(words.decks.at(0).cue == bits(10.5));
    REQUIRE(words.decks.at(1).track_id == 12);
    REQUIRE(words.decks.at(1).flags == 0);
    REQUIRE(words.decks.at(1).position == bits(2.25));
    REQUIRE(words.decks.at(1).rate == bits(-1.25));
    REQUIRE(words.decks.at(1).cue == bits(20.25));
    REQUIRE(words.decks.at(2).flags == ks::ipc::kResumePlaying);
    REQUIRE(words.decks.at(2).rate == bits(3.5));
    REQUIRE(words.decks.at(3).track_id == 0);
    REQUIRE(words.decks.at(3).flags == 0);
    REQUIRE(words.decks.at(3).position == 0);
    REQUIRE(words.decks.at(3).rate == bits(0.75));
    REQUIRE(words.decks.at(3).cue == 0);

    REQUIRE(words.channels.at(0).trim_db == bits(-1.5));
    REQUIRE(words.channels.at(0).fader == bits(0.1));
    REQUIRE(words.channels.at(0).side == 0);
    REQUIRE(words.channels.at(0).cue == 1);
    REQUIRE(words.channels.at(1).side == 1);
    REQUIRE(words.channels.at(1).cue == 0);
    REQUIRE(words.channels.at(2).side == 2);
    REQUIRE(words.channels.at(3).trim_db == bits(4.5));
    REQUIRE(words.channels.at(3).fader == bits(0.4));
    REQUIRE(words.crossfader == bits(0.6));
    REQUIRE(words.crossfader_transition == bits(0.7));
    REQUIRE(words.main_level_db == bits(-5.5));
    REQUIRE(words.headphone_level_db == bits(-6.5));
    REQUIRE(words.cue_mix == bits(0.8));
    REQUIRE(words.bus_main == 4);
    REQUIRE(words.bus_cue == 6);

    // The checksum: the CRC of every word before it, in its low 32 bits.
    const auto bytes = std::bit_cast<std::array<std::byte, sizeof(ResumeState)>>(words);
    const std::uint32_t crc =
        ks::ipc::crc32(std::span{bytes}.first(offsetof(ResumeState, checksum)));
    REQUIRE(words.checksum == crc);

    INFO(word_differences(words, expected_words(snapshot)));
    REQUIRE(same_words(words, expected_words(snapshot)));
}

TEST_CASE("Resume state: valid snapshots round-trip", "[resume][snapshot]") {
    SECTION("distinct values, in either slot") {
        require_round_trip(sample_snapshot(12'345));
        require_round_trip(sample_snapshot(12'346));
    }

    SECTION("generations at their limits, and host time 0") {
        for (const std::uint64_t generation :
             {std::uint64_t{1}, std::uint64_t{2}, kMaxGeneration - 1, kMaxGeneration}) {
            Snapshot snapshot = sample_snapshot(generation);
            snapshot.host_time = at_ns(0);
            require_round_trip(snapshot);
        }
    }

    SECTION("decks at their limits") {
        Snapshot snapshot = sample_snapshot(7);
        snapshot.decks.at(0) = {
            .track_id = 1, .playing = true, .position = 0.0, .rate = 4.0, .cue = 0.0};
        snapshot.decks.at(1) = {.track_id = std::numeric_limits<std::uint64_t>::max(),
                                .playing = true,
                                .position = kMaxFrames,
                                .rate = -4.0,
                                .cue = kMaxFrames};
        snapshot.decks.at(2) = {
            .track_id = 5, .playing = false, .position = 0.25, .rate = 0.0, .cue = kMaxFrames};
        snapshot.decks.at(3) = {
            .track_id = 0, .playing = false, .position = 0.0, .rate = -4.0, .cue = 0.0};
        require_round_trip(snapshot);
    }

    SECTION("the mixer at its limits") {
        Snapshot snapshot = sample_snapshot(8);
        ks::engine::MixerSettings& mixer = snapshot.mixer;
        mixer.channels.at(0) = {
            .trim_db = -kInfinity, .fader = 0.0, .side = CrossfaderSide::A, .cue = false};
        mixer.channels.at(1) = {.trim_db = ks::engine::kMaxTrimDb,
                                .fader = 1.0,
                                .side = CrossfaderSide::B,
                                .cue = true};
        mixer.channels.at(2) = {
            .trim_db = -1e300, .fader = 1.0, .side = CrossfaderSide::Thru, .cue = true};
        mixer.crossfader = 0.0;
        mixer.crossfader_transition = ks::engine::kMinCrossfaderTransition;
        mixer.main_level_db = -kInfinity;
        mixer.headphone_level_db = 0.0;
        mixer.cue_mix = 1.0;
        mixer.bus_map = BusMap{.main = 6, .cue = 0};
        require_round_trip(snapshot);

        mixer.crossfader = 1.0;
        mixer.crossfader_transition = 1.0;
        mixer.main_level_db = 0.0;
        mixer.headphone_level_db = -kInfinity;
        mixer.cue_mix = 0.0;
        mixer.bus_map = BusMap{.main = 0, .cue = 6};
        require_round_trip(snapshot);
    }

    SECTION("the defaults, with every deck empty") {
        Snapshot defaults;
        defaults.generation = 1;
        require_round_trip(defaults);
    }
}

TEST_CASE("Resume state: every corruption is rejected", "[resume][snapshot]") {
    // Generation 6: slot 0.
    const Snapshot snapshot = sample_snapshot(6);
    const ResumeState base = expected_words(snapshot);
    // Without this, a validator that accepts nothing would pass.
    REQUIRE(ks::engine::validate(base, 0) == std::optional<Snapshot>{snapshot});

    const auto rejected = [](const ResumeState& words) {
        return !ks::engine::validate(words, 0).has_value() &&
               !ks::engine::validate(words, 1).has_value();
    };

    SECTION("every single-bit flip of every word") {
        using Words = ks::engine::resume_test::Words;
        const Words good = ks::engine::resume_test::words_of(base);
        std::size_t accepted = 0;
        for (std::size_t word = 0; word < good.size(); ++word) {
            for (unsigned bit = 0; bit < 64; ++bit) {
                Words flipped = good;
                flipped.at(word) ^= std::uint64_t{1} << bit;
                if (!rejected(std::bit_cast<ResumeState>(flipped))) {
                    UNSCOPED_INFO("word " << word << ", bit " << bit);
                    ++accepted;
                }
            }
        }
        REQUIRE(accepted == 0);
    }

    SECTION("the wrong slot, and slot 2") {
        REQUIRE_FALSE(ks::engine::validate(base, 1).has_value());
        REQUIRE_FALSE(ks::engine::validate(base, 2).has_value());
    }

    SECTION("generations 0 and 2^63") {
        ResumeState words = base;
        words.generation = 0;
        REQUIRE(rejected(with_checksum(words)));
        words.generation = std::uint64_t{1} << 63U;
        REQUIRE(rejected(with_checksum(words)));
    }

    SECTION("a checksum with a high bit set") {
        for (const unsigned bit : {32U, 47U, 63U}) {
            ResumeState words = base;
            words.checksum |= std::uint64_t{1} << bit;
            REQUIRE(rejected(words));
        }
    }

    // Each of these changes a word to a value requirement 7 or 9 doesn't
    // allow, and corrects the checksum, so only the range checks can catch it.
    const auto forged = [&](const std::function<void(ResumeState&)>& change) {
        ResumeState words = base;
        change(words);
        return rejected(with_checksum(words));
    };

    SECTION("negative host times") {
        REQUIRE(forged(
            [](ResumeState& w) { w.host_time = std::bit_cast<std::uint64_t>(std::int64_t{-1}); }));
        REQUIRE(forged([](ResumeState& w) {
            w.host_time = std::bit_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::min());
        }));
    }

    SECTION("flags other than bit 0") {
        for (const std::uint64_t flags :
             {std::uint64_t{2}, std::uint64_t{3}, std::uint64_t{1} << 63U}) {
            REQUIRE(forged([flags](ResumeState& w) { w.decks.at(0).flags = flags; }));
        }
    }

    SECTION("an empty deck that's playing, or has a position or a cue point") {
        REQUIRE(forged([](ResumeState& w) { w.decks.at(3).flags = ks::ipc::kResumePlaying; }));
        REQUIRE(forged([](ResumeState& w) { w.decks.at(3).position = bits(1.0); }));
        REQUIRE(forged([](ResumeState& w) { w.decks.at(3).cue = bits(1.0); }));
    }

    SECTION("positions and cue points out of range") {
        for (const double value : {kNaN, kInfinity, -kInfinity, -1.0, -0.5, kMaxFrames + 1.0,
                                   std::nextafter(kMaxFrames, kInfinity)}) {
            INFO("value " << value);
            REQUIRE(forged([value](ResumeState& w) { w.decks.at(0).position = bits(value); }));
            REQUIRE(forged([value](ResumeState& w) { w.decks.at(1).cue = bits(value); }));
        }
    }

    SECTION("rates out of range") {
        for (const double value :
             {kNaN, kInfinity, -kInfinity, std::nextafter(4.0, kInfinity), -4.5, 100.0}) {
            INFO("value " << value);
            REQUIRE(forged([value](ResumeState& w) { w.decks.at(2).rate = bits(value); }));
            // Empty decks' rates are checked too.
            REQUIRE(forged([value](ResumeState& w) { w.decks.at(3).rate = bits(value); }));
        }
    }

    SECTION("trims and levels out of range") {
        for (const double value : {kNaN, kInfinity, 12.5, std::nextafter(12.0, kInfinity)}) {
            INFO("trim " << value);
            REQUIRE(forged([value](ResumeState& w) { w.channels.at(1).trim_db = bits(value); }));
        }
        for (const double value : {kNaN, kInfinity, 0.5, 1e-9}) {
            INFO("level " << value);
            REQUIRE(forged([value](ResumeState& w) { w.main_level_db = bits(value); }));
            REQUIRE(forged([value](ResumeState& w) { w.headphone_level_db = bits(value); }));
        }
    }

    SECTION("faders, the crossfader, and the cue mix outside [0, 1]") {
        for (const double value : {kNaN, -0.1, 1.1, -kInfinity, kInfinity}) {
            INFO("value " << value);
            REQUIRE(forged([value](ResumeState& w) { w.channels.at(2).fader = bits(value); }));
            REQUIRE(forged([value](ResumeState& w) { w.crossfader = bits(value); }));
            REQUIRE(forged([value](ResumeState& w) { w.cue_mix = bits(value); }));
        }
    }

    SECTION("transitions outside [0.01, 1]") {
        for (const double value : {kNaN, 0.005, 0.0, 1.5}) {
            INFO("value " << value);
            REQUIRE(forged([value](ResumeState& w) { w.crossfader_transition = bits(value); }));
        }
    }

    SECTION("sides and channel cues out of range") {
        REQUIRE(forged([](ResumeState& w) { w.channels.at(0).side = 3; }));
        REQUIRE(forged([](ResumeState& w) { w.channels.at(0).side = 99; }));
        REQUIRE(forged([](ResumeState& w) { w.channels.at(3).cue = 2; }));
    }

    SECTION("bus maps that are odd, equal, or 8 or more") {
        for (const auto& [main, cue] : std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                 {1, 2}, {0, 3}, {2, 2}, {8, 0}, {0, 10}, {1, 3}}) {
            INFO("bus map " << main << ", " << cue);
            REQUIRE(forged([main, cue](ResumeState& w) {
                w.bus_main = main;
                w.bus_cue = cue;
            }));
        }
    }
}

TEST_CASE("Resume state: the newest valid slot wins", "[resume][snapshot]") {
    using ks::engine::resume_test::set_slot_words;
    auto state = std::make_unique<ks::ipc::EngineState>();
    const Snapshot older = sample_snapshot(6);
    const Snapshot newer = sample_snapshot(7);
    // Generation 6 belongs in slot 0, and 7 in slot 1.
    set_slot_words(state->resume.at(0), expected_words(older));
    set_slot_words(state->resume.at(1), expected_words(newer));
    state->resume.at(0).sequence = 40;
    state->resume.at(1).sequence = 42;

    SECTION("both valid: the higher generation") {
        REQUIRE(ks::engine::read_snapshot(*state) == std::optional<Snapshot>{newer});
    }

    SECTION("the higher generation in slot 0") {
        const Snapshot newest = sample_snapshot(8);
        set_slot_words(state->resume.at(0), expected_words(newest));
        REQUIRE(ks::engine::read_snapshot(*state) == std::optional<Snapshot>{newest});
    }

    SECTION("the newest corrupted: the older") {
        ResumeState words = expected_words(newer);
        words.decks.at(0).position ^= 1;
        set_slot_words(state->resume.at(1), words);
        REQUIRE(ks::engine::read_snapshot(*state) == std::optional<Snapshot>{older});
    }

    SECTION("the newest left mid-write: the older") {
        state->resume.at(1).sequence = 43;
        REQUIRE(ks::engine::read_snapshot(*state) == std::optional<Snapshot>{older});
    }

    SECTION("neither valid: nothing") {
        state->resume.at(1).sequence = 43;
        ResumeState words = expected_words(older);
        words.checksum ^= 1;
        set_slot_words(state->resume.at(0), words);
        REQUIRE_FALSE(ks::engine::read_snapshot(*state).has_value());
    }

    SECTION("a zero-filled state block: nothing") {
        auto empty = std::make_unique<ks::ipc::EngineState>();
        REQUIRE_FALSE(ks::engine::read_snapshot(*empty).has_value());
    }

    SECTION("it changes nothing") {
        REQUIRE(ks::engine::resume_test::read_check(*state));
    }
}

TEST_CASE("Resume state: validation properties", "[resume][snapshot]") {
    // Requirement 19's checks, as the fuzz target runs them, on 100,000
    // inputs from a fixed seed.
    std::mt19937_64 random{1};
    std::uniform_int_distribution<std::size_t> length{0, 800};
    std::uniform_int_distribution<unsigned> byte{0, 255};
    auto state = std::make_unique<ks::ipc::EngineState>();
    std::vector<std::uint8_t> input;
    input.reserve(800);
    std::size_t failures = 0;
    for (int i = 0; i < 100'000; ++i) {
        input.resize(length(random));
        for (std::uint8_t& b : input) {
            b = static_cast<std::uint8_t>(byte(random));
        }
        if (!ks::engine::resume_test::check_input(input, *state)) {
            ++failures;
        }
    }
    REQUIRE(failures == 0);
}
