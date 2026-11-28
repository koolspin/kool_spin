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
#include <optional>
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

/// The active driver thread's scheduling policy (specs/health-checks/spec.md
/// requirement 6).
enum class Policy : std::uint8_t { Unknown = 0, Other = 1, Fifo = 2, Rr = 3, TimeConstraint = 4 };

/// The last mlockall()'s outcome: none when not tried, or not available.
enum class ProcessLock : std::uint8_t { None = 0, Locked = 1, Failed = 2 };

enum class PowerProfile : std::uint8_t { Unknown = 0, Normal = 1, Saving = 2 };

enum class PowerSource : std::uint8_t { Unknown = 0, Mains = 1, Battery = 2 };

/// Whether the machine is kept awake (step 10b).
enum class Awake : std::uint8_t { Unknown = 0, Blocked = 1, NotBlocked = 2 };

/// The engine's last health pass (specs/health-checks/spec.md requirements
/// 12 and 14). A code the client doesn't know reads as unknown (none, for
/// `process_lock`), and `failing` holds only checks in `known`.
struct HealthInfo {
    /// Health passes so far; never 0 here.
    std::uint64_t passes = 0;
    std::uint64_t host_time = 0;
    /// ipc::kHealthRealtime ... ipc::kHealthAwake: the checks that could
    /// tell, and those that fail.
    std::uint64_t known = 0;
    std::uint64_t failing = 0;
    Policy policy = Policy::Unknown;
    std::uint64_t priority = 0;
    std::uint64_t rtprio_limit = 0;
    /// ipc::kUnlimited for unlimited.
    std::uint64_t memlock_limit = 0;
    ProcessLock process_lock = ProcessLock::None;
    std::uint64_t locked_bytes = 0;
    std::uint64_t unlocked_bytes = 0;
    PowerProfile power_profile = PowerProfile::Unknown;
    PowerSource power_source = PowerSource::Unknown;
    double peak_load = 0.0;
    Awake awake = Awake::Unknown;

    friend bool operator==(const HealthInfo&, const HealthInfo&) = default;
};

/// The health block, validated: none while it says no pass has run.
[[nodiscard]] std::optional<HealthInfo> health_info(const ipc::HealthState& health) noexcept;

/// The whole state. Each block is consistent in itself (read through its
/// seqlock); different blocks may come from different moments.
struct Snapshot {
    EngineInfo engine;
    std::array<DeckInfo, ipc::kStateDecks> decks;
    std::array<ChannelInfo, ipc::kStateDecks> channels;
    MixerInfo mixer;
    std::array<ClientInfo, ipc::kStateClients> clients;
    /// The engine's last health pass; none before its first.
    std::optional<HealthInfo> health;
};

} // namespace ks::client
