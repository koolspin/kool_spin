// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Snapshots: what the engine needs to resume after a crash, and the resume
/// slots they're written to and read from (specs/resume-state/spec.md
/// requirements 6 to 10; ADR 0003 §11).

#include <ks/core/realtime.h>
#include <ks/engine/mixer.h>
#include <ks/ipc/state.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ks::engine {

/// The highest valid generation: 2⁶³ − 1 (requirement 7).
inline constexpr std::uint64_t kMaxGeneration = (std::uint64_t{1} << 63U) - 1;

/// One deck in a snapshot (requirement 6). The default is an empty deck.
struct DeckSnapshot {
    /// The main head's track's ID; 0 for none.
    std::uint64_t track_id = 0;
    bool playing = false;
    /// The main head's position, in track frames.
    double position = 0.0;
    /// The target rate.
    double rate = 1.0;
    /// The cue point, in track frames.
    double cue = 0.0;

    bool operator==(const DeckSnapshot&) const = default;
};

/// What the engine needs to resume, as of the end of a block: the engine's
/// own, validated copy of a resume slot (requirement 6).
struct Snapshot {
    /// Counts snapshots written, across engines; valid from 1 to
    /// kMaxGeneration. A snapshot belongs in slot generation % 2.
    std::uint64_t generation = 0;
    /// The end of the block it describes.
    std::chrono::steady_clock::time_point host_time;
    std::array<DeckSnapshot, kDecks> decks{};
    MixerSettings mixer{};

    bool operator==(const Snapshot&) const = default;
};

/// The resume-slot words for a valid snapshot, with their checksum
/// (requirement 8).
[[nodiscard]] ipc::ResumeState encode(const Snapshot& snapshot) noexcept KS_NONBLOCKING;

/// The snapshot `words` encode, if they're a valid snapshot for resume slot
/// `slot`, and nothing otherwise (requirement 9). A pure function of its
/// arguments, safe for any bytes. For any words it accepts, encode() of the
/// result gives back the same words.
[[nodiscard]] std::optional<Snapshot> validate(const ipc::ResumeState& words,
                                               std::size_t slot) noexcept KS_NONBLOCKING;

/// Copies each of `state`'s resume slots once, validates the copies, and
/// returns the valid snapshot with the higher generation, or nothing if
/// neither slot is valid (requirement 10). A slot left mid-write is invalid.
/// It writes nothing. For building an engine, on the control thread.
[[nodiscard]] std::optional<Snapshot> read_snapshot(ipc::EngineState& state) noexcept;

} // namespace ks::engine
