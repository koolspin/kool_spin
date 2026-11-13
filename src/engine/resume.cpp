// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/resume.h>

#include <ks/engine/block.h>
#include <ks/engine/deck.h>
#include <ks/engine/mixer.h>
#include <ks/engine/track.h>
#include <ks/ipc/crc32.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>

#include "codes.h"

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ks::engine {

namespace {

/// The CRC-32 of every word of `words` before its checksum (requirement 8).
std::uint64_t checksum_of(const ipc::ResumeState& words) noexcept KS_NONBLOCKING {
    const std::span<const std::byte> bytes =
        std::as_bytes(std::span{&words, 1}).first(offsetof(ipc::ResumeState, checksum));
    return ipc::crc32(bytes);
}

/// The inverse of codes::side_code(): the side a code names, or nothing for a
/// code MixerState doesn't allow.
std::optional<CrossfaderSide> side_of(std::uint64_t code) noexcept KS_NONBLOCKING {
    switch (code) {
    case 0:
        return CrossfaderSide::A;
    case 1:
        return CrossfaderSide::B;
    case 2:
        return CrossfaderSide::Thru;
    default:
        return std::nullopt;
    }
}

/// The inverse of codes::flag_code(): 0 or 1, and nothing for another code.
std::optional<bool> flag_of(std::uint64_t code) noexcept KS_NONBLOCKING {
    switch (code) {
    case 0:
        return false;
    case 1:
        return true;
    default:
        return std::nullopt;
    }
}

/// A real from its bit pattern.
double real_of(std::uint64_t code) noexcept KS_NONBLOCKING {
    return std::bit_cast<double>(code);
}

/// Whether `x` is in [lo, hi]. False for NaN.
bool within(double x, double lo, double hi) noexcept KS_NONBLOCKING {
    return x >= lo && x <= hi;
}

/// Whether `deck` is a valid deck in a snapshot (requirement 7).
bool valid(const DeckSnapshot& deck) noexcept KS_NONBLOCKING {
    if (!within(deck.rate, -kMaxRate, kMaxRate)) {
        return false;
    }
    if (deck.track_id == 0) {
        return !deck.playing && deck.position == 0.0 && deck.cue == 0.0;
    }
    const auto max_frames = static_cast<double>(kMaxTrackFrames);
    return within(deck.position, 0.0, max_frames) && within(deck.cue, 0.0, max_frames);
}

/// Whether a Mixer can hold `mixer` (requirement 7). −∞ trims and levels are
/// allowed; NaN and +∞ aren't.
/// Whether `map` is valid: both channels even, below kMaxOutputChannels, and
/// different. The same check as Mixer's, which is private.
bool valid(const BusMap& map) noexcept KS_NONBLOCKING {
    return map.main % 2 == 0 && map.cue % 2 == 0 && map.main < kMaxOutputChannels &&
           map.cue < kMaxOutputChannels && map.main != map.cue;
}

bool valid(const ChannelSettings& channel) noexcept KS_NONBLOCKING {
    return channel.trim_db <= kMaxTrimDb && within(channel.fader, 0.0, 1.0);
}

bool valid(const MixerSettings& mixer) noexcept KS_NONBLOCKING {
    for (const ChannelSettings& channel : mixer.channels) {
        if (!valid(channel)) {
            return false;
        }
    }
    return within(mixer.crossfader, 0.0, 1.0) &&
           within(mixer.crossfader_transition, kMinCrossfaderTransition, 1.0) &&
           mixer.main_level_db <= 0.0 && mixer.headphone_level_db <= 0.0 &&
           within(mixer.cue_mix, 0.0, 1.0) && valid(mixer.bus_map);
}

/// Whether `snapshot` is valid (requirement 7).
bool valid(const Snapshot& snapshot) noexcept KS_NONBLOCKING {
    if (snapshot.generation < 1 || snapshot.generation > kMaxGeneration) {
        return false;
    }
    if (snapshot.host_time.time_since_epoch().count() < 0) {
        return false;
    }
    for (const DeckSnapshot& deck : snapshot.decks) {
        if (!valid(deck)) {
            return false;
        }
    }
    return valid(snapshot.mixer);
}

} // namespace

ipc::ResumeState encode(const Snapshot& snapshot) noexcept KS_NONBLOCKING {
    ipc::ResumeState words{};
    words.generation = snapshot.generation;
    words.host_time = codes::host_time_code(snapshot.host_time);
    for (std::size_t i = 0; i < kDecks; ++i) {
        const DeckSnapshot& deck = std::span{snapshot.decks}[i];
        ipc::ResumeDeck& out = std::span{words.decks}[i];
        out.track_id = deck.track_id;
        out.flags = deck.playing ? ipc::kResumePlaying : 0U;
        out.position = codes::real_code(deck.position);
        out.rate = codes::real_code(deck.rate);
        out.cue = codes::real_code(deck.cue);
    }
    const MixerSettings& mixer = snapshot.mixer;
    for (std::size_t i = 0; i < kDecks; ++i) {
        const ChannelSettings& channel = std::span{mixer.channels}[i];
        ipc::ChannelState& out = std::span{words.channels}[i];
        out.trim_db = codes::real_code(channel.trim_db);
        out.fader = codes::real_code(channel.fader);
        out.side = codes::side_code(channel.side);
        out.cue = codes::flag_code(channel.cue);
    }
    words.crossfader = codes::real_code(mixer.crossfader);
    words.crossfader_transition = codes::real_code(mixer.crossfader_transition);
    words.main_level_db = codes::real_code(mixer.main_level_db);
    words.headphone_level_db = codes::real_code(mixer.headphone_level_db);
    words.cue_mix = codes::real_code(mixer.cue_mix);
    words.bus_main = mixer.bus_map.main;
    words.bus_cue = mixer.bus_map.cue;
    words.checksum = checksum_of(words);
    return words;
}

std::optional<Snapshot> validate(const ipc::ResumeState& words,
                                 std::size_t slot) noexcept KS_NONBLOCKING {
    // Copy, validate, use: every check below reads this private copy.
    const ipc::ResumeState copy = words;
    if (slot >= ipc::kResumeSlots || copy.generation % ipc::kResumeSlots != slot) {
        return std::nullopt;
    }
    if (copy.checksum != checksum_of(copy)) { // also rejects a high bit set
        return std::nullopt;
    }
    const auto host_ns = std::bit_cast<std::int64_t>(copy.host_time);
    if (host_ns < 0) {
        return std::nullopt;
    }

    Snapshot snapshot;
    snapshot.generation = copy.generation;
    snapshot.host_time = std::chrono::steady_clock::time_point{
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds{host_ns})};
    for (std::size_t i = 0; i < kDecks; ++i) {
        const ipc::ResumeDeck& in = std::span{copy.decks}[i];
        DeckSnapshot& deck = std::span{snapshot.decks}[i];
        if ((in.flags & ~ipc::kResumePlaying) != 0) {
            return std::nullopt;
        }
        deck.track_id = in.track_id;
        deck.playing = (in.flags & ipc::kResumePlaying) != 0;
        deck.position = real_of(in.position);
        deck.rate = real_of(in.rate);
        deck.cue = real_of(in.cue);
    }
    MixerSettings& mixer = snapshot.mixer;
    for (std::size_t i = 0; i < kDecks; ++i) {
        const ipc::ChannelState& in = std::span{copy.channels}[i];
        ChannelSettings& channel = std::span{mixer.channels}[i];
        const std::optional<CrossfaderSide> side = side_of(in.side);
        const std::optional<bool> cue = flag_of(in.cue);
        if (!side || !cue) {
            return std::nullopt;
        }
        channel.trim_db = real_of(in.trim_db);
        channel.fader = real_of(in.fader);
        channel.side = *side;
        channel.cue = *cue;
    }
    mixer.crossfader = real_of(copy.crossfader);
    mixer.crossfader_transition = real_of(copy.crossfader_transition);
    mixer.main_level_db = real_of(copy.main_level_db);
    mixer.headphone_level_db = real_of(copy.headphone_level_db);
    mixer.cue_mix = real_of(copy.cue_mix);
    mixer.bus_map.main = copy.bus_main;
    mixer.bus_map.cue = copy.bus_cue;

    if (!valid(snapshot)) {
        return std::nullopt;
    }
    return snapshot;
}

std::optional<Snapshot> read_snapshot(ipc::EngineState& state) noexcept {
    // Each slot is read once. Generations in different slots differ in
    // parity, so two valid snapshots never tie.
    std::optional<Snapshot> newest;
    std::size_t slot = 0;
    for (auto& lock : state.resume) {
        const std::size_t this_slot = slot++;
        const std::optional<ipc::ResumeState> words =
            ipc::SeqlockReader<ipc::ResumeState>{lock}.try_read();
        if (!words) {
            continue;
        }
        const std::optional<Snapshot> snapshot = validate(*words, this_slot);
        if (snapshot && (!newest || snapshot->generation > newest->generation)) {
            newest = snapshot;
        }
    }
    return newest;
}

} // namespace ks::engine
