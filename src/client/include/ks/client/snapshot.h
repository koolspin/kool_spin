// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// A validated copy of the engine's state (specs/ks-play/spec.md requirement
/// 17): what a client reads from the engine-state segment, with every field
/// checked and in a form that's safe to use. Reals are kept as they are, even
/// NaN or infinite: they're data to show.

#include <ks/ipc/state.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace ks::client {

/// A crossfader side.
enum class Side : std::uint8_t { A = 0, B = 1, Thru = 2 };

/// What the output is doing.
enum class OutputState : std::uint8_t { Stopped = 0, Playing = 1, Freewheeling = 2, Attaching = 3 };

/// Why the engine rejected a command (0 is none).
enum class Rejection : std::uint8_t {
    None = 0,
    UnknownType = 1,
    BadTarget = 2,
    NonzeroPadding = 3,
    BadValue = 4,
};

/// A rejection's name as ks-play writes it: `none`, `unknown type`,
/// `bad target`, `nonzero padding`, or `bad value`.
[[nodiscard]] std::string_view rejection_name(Rejection rejection) noexcept;

struct DeckInfo {
    bool loaded = false;
    bool playing = false;
    bool waiting = false;
    bool complete = false;
    std::uint64_t track_id = 0;
    std::uint64_t track_sample_rate = 0;
    std::uint64_t track_frames = 0;
    std::uint64_t frames_ready = 0;
    double position = 0.0; ///< In track frames.
    double rate = 0.0;
    double cue = 0.0; ///< In track frames.

    friend bool operator==(const DeckInfo&, const DeckInfo&) = default;
};

struct ChannelInfo {
    double trim_db = 0.0;
    double fader = 0.0;
    Side side = Side::Thru;
    bool cue = false;

    friend bool operator==(const ChannelInfo&, const ChannelInfo&) = default;
};

struct MixerInfo {
    double crossfader = 0.0;
    double crossfader_transition = 0.0;
    double main_level_db = 0.0;
    double headphone_level_db = 0.0;
    double cue_mix = 0.0;
    std::uint64_t bus_main = 0;
    std::uint64_t bus_cue = 0;

    friend bool operator==(const MixerInfo&, const MixerInfo&) = default;
};

struct ClientInfo {
    bool attached = false;
    bool corrupt = false;
    std::uint64_t commands_read = 0;
    std::uint64_t last_sequence = 0;
    std::uint64_t rejected = 0;
    Rejection last_rejection = Rejection::None;

    friend bool operator==(const ClientInfo&, const ClientInfo&) = default;
};

struct EngineInfo {
    std::uint64_t heartbeat = 0;
    std::uint64_t frames_rendered = 0;
    std::uint64_t sample_rate = 0;
    OutputState state = OutputState::Stopped;
    bool muted = false;
    bool silenced = false;
    std::uint64_t non_finite_samples = 0;
    std::uint64_t limited_frames = 0;
    std::uint64_t late_blocks = 0;
    double render_load = 0.0;
    double peak_render_load = 0.0;

    friend bool operator==(const EngineInfo&, const EngineInfo&) = default;
};

/// The whole state. Each block is consistent in itself (read through its
/// seqlock); different blocks may come from different moments.
struct Snapshot {
    EngineInfo engine;
    std::array<DeckInfo, ipc::kStateDecks> decks;
    std::array<ChannelInfo, ipc::kStateDecks> channels;
    MixerInfo mixer;
    std::array<ClientInfo, ipc::kStateClients> clients;
};

} // namespace ks::client
