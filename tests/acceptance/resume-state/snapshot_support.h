// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the snapshot tests and the resume-state fuzz target: an encoder
// and a validity check written from specs/resume-state/spec.md requirements
// 3, 7, and 8, independently of encode() and validate(); a generator of valid
// snapshots; and requirement 19's properties.

#include <ks/engine/block.h>
#include <ks/engine/deck.h>
#include <ks/engine/mixer.h>
#include <ks/engine/resume.h>
#include <ks/engine/track.h>
#include <ks/ipc/crc32.h>
#include <ks/ipc/state.h>

#include "../commands/codec_support.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ios>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>

namespace ks::engine::resume_test {

using Clock = std::chrono::steady_clock;
using test::ByteReader;

inline constexpr double kInfinity = std::numeric_limits<double>::infinity();
inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// The words of a resume slot's value.
inline constexpr std::size_t kWords = sizeof(ipc::ResumeState) / sizeof(std::uint64_t);
using Words = std::array<std::uint64_t, kWords>;

inline Words words_of(const ipc::ResumeState& state) {
    return std::bit_cast<Words>(state);
}

inline bool same_words(const ipc::ResumeState& a, const ipc::ResumeState& b) {
    return words_of(a) == words_of(b);
}

/// The words in which `a` and `b` differ, as text: empty if none.
inline std::string word_differences(const ipc::ResumeState& a, const ipc::ResumeState& b) {
    const Words wa = words_of(a);
    const Words wb = words_of(b);
    std::ostringstream out;
    for (std::size_t i = 0; i < kWords; ++i) {
        if (wa.at(i) != wb.at(i)) {
            out << "word " << i << ": 0x" << std::hex << wa.at(i) << " vs 0x" << wb.at(i)
                << std::dec << "; ";
        }
    }
    return out.str();
}

inline std::uint64_t bits(double value) {
    return std::bit_cast<std::uint64_t>(value);
}

/// A host time as the layout stores it.
inline std::uint64_t time_word(Clock::time_point time) {
    const std::int64_t ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    return std::bit_cast<std::uint64_t>(ns);
}

/// The host time `ns` nanoseconds after the steady clock's epoch.
inline Clock::time_point at_ns(std::int64_t ns) {
    return Clock::time_point{
        std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds{ns})};
}

inline std::uint64_t side_code(CrossfaderSide side) {
    switch (side) {
    case CrossfaderSide::A:
        return 0;
    case CrossfaderSide::B:
        return 1;
    case CrossfaderSide::Thru:
        return 2;
    }
    return 99;
}

/// The checksum word `state` should have: the CRC-32 of the bytes of every
/// word before it (requirement 8).
inline std::uint64_t checksum_of(const ipc::ResumeState& state) {
    const auto bytes = std::bit_cast<std::array<std::byte, sizeof(ipc::ResumeState)>>(state);
    return ipc::crc32(std::span{bytes}.first(offsetof(ipc::ResumeState, checksum)));
}

/// `state` with its checksum made right for its other words.
inline ipc::ResumeState with_checksum(ipc::ResumeState state) {
    state.checksum = checksum_of(state);
    return state;
}

/// The words requirement 3 says encode `snapshot`, with the checksum left 0.
inline ipc::ResumeState unchecked_words(const Snapshot& snapshot) {
    ipc::ResumeState words{};
    words.generation = snapshot.generation;
    words.host_time = time_word(snapshot.host_time);
    for (std::size_t i = 0; i < kDecks; ++i) {
        const DeckSnapshot& deck = snapshot.decks.at(i);
        ipc::ResumeDeck& out = words.decks.at(i);
        out.track_id = deck.track_id;
        out.flags = deck.playing ? ipc::kResumePlaying : 0;
        out.position = bits(deck.position);
        out.rate = bits(deck.rate);
        out.cue = bits(deck.cue);
    }
    const MixerSettings& mixer = snapshot.mixer;
    for (std::size_t i = 0; i < kDecks; ++i) {
        const ChannelSettings& channel = mixer.channels.at(i);
        ipc::ChannelState& out = words.channels.at(i);
        out.trim_db = bits(channel.trim_db);
        out.fader = bits(channel.fader);
        out.side = side_code(channel.side);
        out.cue = channel.cue ? 1 : 0;
    }
    words.crossfader = bits(mixer.crossfader);
    words.crossfader_transition = bits(mixer.crossfader_transition);
    words.main_level_db = bits(mixer.main_level_db);
    words.headphone_level_db = bits(mixer.headphone_level_db);
    words.cue_mix = bits(mixer.cue_mix);
    words.bus_main = mixer.bus_map.main;
    words.bus_cue = mixer.bus_map.cue;
    return words;
}

/// The words requirements 3 and 8 say encode `snapshot`.
inline ipc::ResumeState expected_words(const Snapshot& snapshot) {
    return with_checksum(unchecked_words(snapshot));
}

/// Whether two snapshots are the same, bit for bit (so 0.0 and −0.0 differ).
/// Cheaper than comparing their encodings, which needs two CRCs.
inline bool same_snapshot(const Snapshot& a, const Snapshot& b) {
    return same_words(unchecked_words(a), unchecked_words(b));
}

/// Whether `value` is in [low, high]; false for NaN.
inline bool within(double value, double low, double high) {
    return value >= low && value <= high;
}

/// Whether `snapshot` is valid, as requirement 7 says.
inline bool is_valid(const Snapshot& snapshot) {
    if (snapshot.generation < 1 || snapshot.generation > kMaxGeneration) {
        return false;
    }
    if (snapshot.host_time.time_since_epoch().count() < 0) {
        return false;
    }
    const auto max_frames = static_cast<double>(kMaxTrackFrames);
    for (const DeckSnapshot& deck : snapshot.decks) {
        if (!within(deck.rate, -kMaxRate, kMaxRate)) {
            return false;
        }
        if (deck.track_id == 0) {
            if (deck.playing || deck.position != 0.0 || deck.cue != 0.0) {
                return false;
            }
        } else if (!within(deck.position, 0.0, max_frames) || !within(deck.cue, 0.0, max_frames)) {
            return false;
        }
    }
    const MixerSettings& mixer = snapshot.mixer;
    for (const ChannelSettings& channel : mixer.channels) {
        if (std::isnan(channel.trim_db) || channel.trim_db > kMaxTrimDb ||
            !within(channel.fader, 0.0, 1.0) || side_code(channel.side) > 2) {
            return false;
        }
    }
    const BusMap& map = mixer.bus_map;
    return within(mixer.crossfader, 0.0, 1.0) &&
           within(mixer.crossfader_transition, kMinCrossfaderTransition, 1.0) &&
           !std::isnan(mixer.main_level_db) && mixer.main_level_db <= 0.0 &&
           !std::isnan(mixer.headphone_level_db) && mixer.headphone_level_db <= 0.0 &&
           within(mixer.cue_mix, 0.0, 1.0) && map.main % 2 == 0 && map.cue % 2 == 0 &&
           map.main < kMaxOutputChannels && map.cue < kMaxOutputChannels && map.main != map.cue;
}

// A generator of valid snapshots, from the bytes a ByteReader reads.

/// A real in [0, 1).
inline double unit(std::uint64_t raw) {
    return std::ldexp(static_cast<double>(raw >> 11U), -53);
}

/// A real in [low, high]: one of the ends, or a value between.
inline double between(ByteReader& in, double low, double high) {
    const std::uint8_t pick = in.byte();
    const std::uint64_t raw = in.word();
    switch (pick % 4) {
    case 0:
        return low;
    case 1:
        return high;
    default:
        return std::min(high, low + ((high - low) * unit(raw)));
    }
}

/// A level in dB at most `high`, sometimes −∞.
inline double level(ByteReader& in, double high) {
    if (in.byte() % 4 == 0) {
        return -kInfinity;
    }
    return between(in, -120.0, high);
}

/// A valid snapshot chosen by the bytes `in` reads, the limits of requirement
/// 7 included.
inline Snapshot valid_snapshot(ByteReader& in) {
    Snapshot snapshot;
    const std::uint8_t pick = in.byte();
    const std::uint64_t raw = in.word();
    switch (pick % 4) {
    case 0:
        snapshot.generation = 1;
        break;
    case 1:
        snapshot.generation = 2;
        break;
    case 2:
        snapshot.generation = kMaxGeneration;
        break;
    default:
        snapshot.generation = 1 + (raw % kMaxGeneration);
        break;
    }
    const std::uint64_t time = in.word();
    snapshot.host_time = at_ns((pick & 4U) != 0 ? 0 : static_cast<std::int64_t>(time >> 1U));

    const auto max_frames = static_cast<double>(kMaxTrackFrames);
    for (DeckSnapshot& deck : snapshot.decks) {
        const std::uint8_t kind = in.byte();
        const std::uint64_t id = in.word();
        deck.rate = between(in, -kMaxRate, kMaxRate);
        if (kind % 3 == 0) {
            continue; // empty: ID 0, paused, position 0, cue 0
        }
        deck.track_id = id == 0 ? 1 : id;
        deck.playing = (kind & 4U) != 0;
        deck.position = between(in, 0.0, max_frames);
        deck.cue = between(in, 0.0, max_frames);
    }

    MixerSettings& mixer = snapshot.mixer;
    for (ChannelSettings& channel : mixer.channels) {
        channel.trim_db = level(in, kMaxTrimDb);
        channel.fader = between(in, 0.0, 1.0);
        const std::uint8_t flags = in.byte();
        constexpr std::array<CrossfaderSide, 3> kSides{CrossfaderSide::A, CrossfaderSide::B,
                                                       CrossfaderSide::Thru};
        channel.side = kSides.at(flags % 3);
        channel.cue = (flags & 8U) != 0;
    }
    mixer.crossfader = between(in, 0.0, 1.0);
    mixer.crossfader_transition = between(in, kMinCrossfaderTransition, 1.0);
    mixer.main_level_db = level(in, 0.0);
    mixer.headphone_level_db = level(in, 0.0);
    mixer.cue_mix = between(in, 0.0, 1.0);
    // Distinct even channels below kMaxOutputChannels.
    constexpr std::size_t kPairs = kMaxOutputChannels / 2;
    const std::uint64_t map = in.word();
    const std::size_t main = map % kPairs;
    const std::size_t cue = (main + 1 + ((map >> 8U) % (kPairs - 1))) % kPairs;
    mixer.bus_map = BusMap{.main = 2 * main, .cue = 2 * cue};
    return snapshot;
}

// Requirement 19's properties.

/// Words to snapshot (requirement 9): whatever validate() accepts, for slot 0
/// or 1, is a valid snapshot of that slot's parity that encodes back to the
/// same words, and slot 2 accepts nothing.
inline bool words_check(const ipc::ResumeState& words) {
    for (std::size_t slot = 0; slot < ipc::kResumeSlots; ++slot) {
        const std::optional<Snapshot> snapshot = validate(words, slot);
        if (!snapshot.has_value()) {
            continue;
        }
        // Every word but the checksum as requirement 3 says, and the
        // checksum as encode() computes it.
        ipc::ResumeState wanted = unchecked_words(*snapshot);
        wanted.checksum = words.checksum;
        if (!is_valid(*snapshot) || snapshot->generation % 2 != slot ||
            !same_words(wanted, words) || !same_words(encode(*snapshot), words)) {
            return false;
        }
    }
    return !validate(words, ipc::kResumeSlots).has_value();
}

/// Snapshot to words (requirement 9): a valid snapshot encodes as requirement
/// 3 says, validates back to itself, bit for bit, in its own slot, and to
/// nothing in the other.
inline bool snapshot_round_trips(const Snapshot& snapshot) {
    const ipc::ResumeState words = encode(snapshot);
    if (!same_words(words, expected_words(snapshot))) {
        return false;
    }
    const std::size_t slot = snapshot.generation % 2;
    const std::optional<Snapshot> back = validate(words, slot);
    return back.has_value() && same_snapshot(*back, snapshot) &&
           !validate(words, 1 - slot).has_value();
}

/// The words a seqlock holds, as a plain copy. Single-threaded only.
inline ipc::ResumeState slot_words(const ipc::Seqlock<ipc::ResumeState>& lock) {
    return std::bit_cast<ipc::ResumeState>(lock.words);
}

/// Sets the words a seqlock holds. Single-threaded only.
inline void set_slot_words(ipc::Seqlock<ipc::ResumeState>& lock, const ipc::ResumeState& words) {
    lock.words = std::bit_cast<decltype(lock.words)>(words);
}

/// Requirement 10: read_snapshot() returns the newer of the slots whose
/// sequence number is even and whose words validate() accepts, or nothing,
/// and changes nothing.
inline bool read_check(ipc::EngineState& state) {
    std::optional<Snapshot> newest;
    for (std::size_t slot = 0; slot < ipc::kResumeSlots; ++slot) {
        const ipc::Seqlock<ipc::ResumeState>& lock = state.resume.at(slot);
        if (lock.sequence % 2 != 0) {
            continue;
        }
        const std::optional<Snapshot> snapshot = validate(slot_words(lock), slot);
        if (snapshot.has_value() &&
            (!newest.has_value() || snapshot->generation > newest->generation)) {
            newest = snapshot;
        }
    }
    const auto before = std::bit_cast<std::array<std::byte, sizeof(state.resume)>>(state.resume);
    const std::optional<Snapshot> read = read_snapshot(state);
    const auto after = std::bit_cast<std::array<std::byte, sizeof(state.resume)>>(state.resume);
    if (before != after || read.has_value() != newest.has_value()) {
        return false;
    }
    return !read.has_value() || same_snapshot(*read, *newest);
}

/// Fills both resume slots from the bytes `in` reads: each slot's sequence
/// number, even or odd, and words that are arbitrary, a valid snapshot of the
/// slot's parity, or zero.
inline void fill_slots(ipc::EngineState& state, ByteReader& in) {
    for (std::size_t slot = 0; slot < ipc::kResumeSlots; ++slot) {
        ipc::Seqlock<ipc::ResumeState>& lock = state.resume.at(slot);
        const std::uint8_t kind = in.byte();
        const std::uint64_t sequence = in.word();
        lock.sequence = (kind & 1U) != 0 ? sequence : (sequence & ~std::uint64_t{1});
        ipc::ResumeState words{};
        switch ((kind >> 1U) % 3) {
        case 0: {
            Words raw{};
            for (std::uint64_t& word : raw) {
                word = in.word();
            }
            words = std::bit_cast<ipc::ResumeState>(raw);
            break;
        }
        case 1: {
            Snapshot snapshot = valid_snapshot(in);
            snapshot.generation = (snapshot.generation & ~std::uint64_t{1}) | slot;
            if (snapshot.generation == 0) {
                snapshot.generation = 2;
            }
            words = expected_words(snapshot);
            break;
        }
        default:
            break;
        }
        set_slot_words(lock, words);
    }
}

/// Requirement 19: every property, for one input. `state` is scratch space.
inline bool check_input(std::span<const std::uint8_t> input, ipc::EngineState& state) {
    // The input's bytes as a slot's words, zero-padded, as they are and with
    // the checksum corrected, so the range checks are reached.
    ipc::ResumeState words{};
    if (!input.empty()) {
        std::memcpy(&words, input.data(), std::min(input.size(), sizeof words));
    }
    if (!words_check(words) || !words_check(with_checksum(words))) {
        return false;
    }
    // A valid snapshot chosen by the input.
    ByteReader snapshot_bytes{input};
    if (!snapshot_round_trips(valid_snapshot(snapshot_bytes))) {
        return false;
    }
    // Both slots, sequence numbers included.
    ByteReader slot_bytes{input};
    fill_slots(state, slot_bytes);
    return read_check(state);
}

} // namespace ks::engine::resume_test
