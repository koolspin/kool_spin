// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/resume.h>

#include <ks/engine/mixer.h>
#include <ks/ipc/crc32.h>
#include <ks/ipc/state.h>

#include "codes.h"

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

// Stubs (specs/resume-state/spec.md requirements 9 and 10): nothing validates,
// and no slot is read.

std::optional<Snapshot> validate(const ipc::ResumeState& /*words*/,
                                 std::size_t /*slot*/) noexcept KS_NONBLOCKING {
    return std::nullopt;
}

std::optional<Snapshot> read_snapshot(ipc::EngineState& /*state*/) noexcept {
    return std::nullopt;
}

} // namespace ks::engine
