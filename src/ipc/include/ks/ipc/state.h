// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The engine-state segment: what the engine publishes after every block it
/// renders (ADR 0003 §5, specs/state-out/spec.md requirements 1 to 8), and the
/// two resume slots it writes in turn (ADR 0003 §11,
/// specs/resume-state/spec.md requirements 2 to 5), and the health block its
/// control thread writes after each health pass (specs/health-checks/spec.md
/// requirement 12).
///
/// Each block is a seqlock that only the engine writes: the health block from
/// its control thread, the others from its audio thread. Every field is a
/// std::uint64_t:
/// - reals are the bit pattern of an IEEE-754 double;
/// - host times are nanoseconds since the steady clock's epoch, as the bit
///   pattern of a std::int64_t; durations are nanoseconds;
/// - flags are the bits listed below, and other bits are 0;
/// - a crossfader side is 0 for A, 1 for B, and 2 for thru; an output state is
///   0 stopped, 1 playing, 2 freewheeling, 3 attaching; a rejection is 0 for
///   none, then 1 UnknownType, 2 BadTarget, 3 NonzeroPadding, 4 BadValue.

#include <ks/ipc/region.h>
#include <ks/ipc/seqlock.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ks::ipc {

/// Decks the state segment describes: engine::kDecks.
inline constexpr std::size_t kStateDecks = 4;
/// Client slots the state segment describes: engine::kMaxClients.
inline constexpr std::size_t kStateClients = 8;

/// DeckState::flags: a track is on the main head.
inline constexpr std::uint64_t kDeckLoaded = std::uint64_t{1} << 0U;
/// DeckState::flags: playing.
inline constexpr std::uint64_t kDeckPlaying = std::uint64_t{1} << 1U;
/// DeckState::flags: waiting at the decode limit.
inline constexpr std::uint64_t kDeckWaiting = std::uint64_t{1} << 2U;
/// DeckState::flags: the main head's track is completely decoded.
inline constexpr std::uint64_t kDeckComplete = std::uint64_t{1} << 3U;

/// ClientAck::flags: a ring is attached to the slot.
inline constexpr std::uint64_t kClientAttached = std::uint64_t{1} << 0U;
/// ClientAck::flags: the slot's ring was found corrupt, and isn't read.
inline constexpr std::uint64_t kClientCorrupt = std::uint64_t{1} << 1U;

/// EngineStatus::flags: the output is muted.
inline constexpr std::uint64_t kStatusMuted = std::uint64_t{1} << 0U;
/// EngineStatus::flags: muted, and the fade has reached silence.
inline constexpr std::uint64_t kStatusSilenced = std::uint64_t{1} << 1U;

/// ResumeDeck::flags: playing.
inline constexpr std::uint64_t kResumePlaying = std::uint64_t{1} << 0U;

/// The resume slots: the engine writes them in turn.
inline constexpr std::size_t kResumeSlots = 2;

// HealthState::known and ::failing: one bit per check
// (specs/health-checks/spec.md requirement 12).
inline constexpr std::uint64_t kHealthRealtime = std::uint64_t{1} << 0U;
inline constexpr std::uint64_t kHealthMemory = std::uint64_t{1} << 1U;
inline constexpr std::uint64_t kHealthPowerProfile = std::uint64_t{1} << 2U;
inline constexpr std::uint64_t kHealthPowerSource = std::uint64_t{1} << 3U;
inline constexpr std::uint64_t kHealthLoad = std::uint64_t{1} << 4U;
/// Step 10b: keeping the machine awake.
inline constexpr std::uint64_t kHealthAwake = std::uint64_t{1} << 5U;
/// Every check's bit.
inline constexpr std::uint64_t kHealthChecks = (std::uint64_t{1} << 6U) - 1;

// HealthState's codes.
/// HealthState::policy: the active driver thread's scheduling.
inline constexpr std::uint64_t kPolicyUnknown = 0;
inline constexpr std::uint64_t kPolicyOther = 1;
inline constexpr std::uint64_t kPolicyFifo = 2;
inline constexpr std::uint64_t kPolicyRr = 3;
inline constexpr std::uint64_t kPolicyTimeConstraint = 4;
/// HealthState::process_lock.
inline constexpr std::uint64_t kProcessLockNone = 0;
inline constexpr std::uint64_t kProcessLockLocked = 1;
inline constexpr std::uint64_t kProcessLockFailed = 2;
/// HealthState::power_profile.
inline constexpr std::uint64_t kPowerProfileUnknown = 0;
inline constexpr std::uint64_t kPowerProfileNormal = 1;
inline constexpr std::uint64_t kPowerProfileSaving = 2;
/// HealthState::power_source.
inline constexpr std::uint64_t kPowerSourceUnknown = 0;
inline constexpr std::uint64_t kPowerSourceMains = 1;
inline constexpr std::uint64_t kPowerSourceBattery = 2;
/// HealthState::awake (step 10b).
inline constexpr std::uint64_t kAwakeUnknown = 0;
inline constexpr std::uint64_t kAwakeBlocked = 1;
inline constexpr std::uint64_t kAwakeNotBlocked = 2;
/// HealthState::memlock_limit when the limit is unlimited.
inline constexpr std::uint64_t kUnlimited = ~std::uint64_t{0};

// The ranges a resume slot's values fall in (specs/resume-state/spec.md
// requirement 7). The engine's limits are these, and the supervisor checks a
// session journal against them (specs/session-journal/spec.md requirement 1).

/// The fastest a deck plays, forward or in reverse.
inline constexpr double kMaxDeckRate = 4.0;
/// The most a trim boosts, in dB.
inline constexpr double kMaxTrimDb = 12.0;
/// The narrowest crossfader transition: the sharpest cut.
inline constexpr double kMinCrossfaderTransition = 0.01;
/// The most output channels the engine renders: a bus's first channel is
/// below it.
inline constexpr std::size_t kMaxOutputChannels = 8;

/// One deck, after a block (requirement 3).
struct DeckState {
    /// The status heartbeat of the block that published it.
    std::uint64_t heartbeat;
    /// kDeckLoaded, kDeckPlaying, kDeckWaiting, kDeckComplete.
    std::uint64_t flags;
    /// The main head's track's ID; 0 with no track.
    std::uint64_t track_id;
    /// Its sample rate; 0 with no track.
    std::uint64_t track_sample_rate;
    /// Its capacity in frames; 0 with no track.
    std::uint64_t track_frames;
    /// Its decoded frames; 0 with no track.
    std::uint64_t frames_ready;
    /// Real: the main head's position after the block, in track frames.
    std::uint64_t position;
    /// Real: the target rate.
    std::uint64_t rate;
    /// Real: the cue point, in track frames.
    std::uint64_t cue;
    /// Host time: when the frame at `position` is due at the driver.
    std::uint64_t host_time;
    /// Duration: how long after that the frame is heard.
    std::uint64_t output_latency;
};

/// One mixer channel's controls (requirement 4).
struct ChannelState {
    std::uint64_t trim_db; ///< Real.
    std::uint64_t fader;   ///< Real.
    std::uint64_t side;    ///< Crossfader side code.
    std::uint64_t cue;     ///< 0 or 1.
};

/// The mixer's settings, after a block (requirement 4).
struct MixerState {
    /// The status heartbeat of the block that published it.
    std::uint64_t heartbeat;
    /// Channel i is deck i.
    std::array<ChannelState, kStateDecks> channels;
    std::uint64_t crossfader;            ///< Real.
    std::uint64_t crossfader_transition; ///< Real.
    std::uint64_t main_level_db;         ///< Real.
    std::uint64_t headphone_level_db;    ///< Real.
    std::uint64_t cue_mix;               ///< Real.
    std::uint64_t bus_main;              ///< The main bus's first device channel.
    std::uint64_t bus_cue;               ///< The cue bus's first device channel.
};

/// What the engine has read from one client slot (requirement 5).
struct ClientAck {
    /// kClientAttached, kClientCorrupt.
    std::uint64_t flags;
    /// Slots read, valid or not.
    std::uint64_t commands_read;
    /// The sequence number of the last slot read; 0 before the first.
    std::uint64_t last_sequence;
    /// Slots the decoder rejected.
    std::uint64_t rejected;
    /// Rejection code of the last rejection; 0 before the first.
    std::uint64_t last_rejection;
};

/// The engine's status, after a block (requirement 6).
struct EngineStatus {
    /// Blocks rendered, this one included.
    std::uint64_t heartbeat;
    std::uint64_t frames_rendered;
    /// The block's sample rate.
    std::uint64_t sample_rate;
    /// Host time: the end of the block.
    std::uint64_t host_time;
    /// Duration: the block's output latency.
    std::uint64_t output_latency;
    /// Output state code.
    std::uint64_t output_state;
    /// kStatusMuted, kStatusSilenced.
    std::uint64_t flags;
    std::uint64_t non_finite_samples;
    std::uint64_t limited_frames;
    /// Rendered blocks that took longer to render than they last.
    std::uint64_t late_blocks;
    /// Duration: how long the block took to render.
    std::uint64_t render_time;
    /// Real: the block's render time divided by its duration.
    std::uint64_t render_load;
    /// Real: the highest render load of any block.
    std::uint64_t peak_render_load;
    /// Slot 0 first.
    std::array<ClientAck, kStateClients> clients;
};

/// One deck in a resume slot (resume-state requirement 2).
struct ResumeDeck {
    /// The main head's track's ID; 0 with no track.
    std::uint64_t track_id;
    /// kResumePlaying.
    std::uint64_t flags;
    /// Real: the main head's position, in track frames.
    std::uint64_t position;
    /// Real: the target rate.
    std::uint64_t rate;
    /// Real: the cue point, in track frames.
    std::uint64_t cue;
};

/// One resume slot's value: what the engine needs to resume, as of the end of
/// a block (resume-state requirement 3).
struct ResumeState {
    /// Counts snapshots written, across engines; 0 in a slot never written.
    std::uint64_t generation;
    /// Host time: the end of the block.
    std::uint64_t host_time;
    std::array<ResumeDeck, kStateDecks> decks;
    /// Channel i is deck i.
    std::array<ChannelState, kStateDecks> channels;
    std::uint64_t crossfader;            ///< Real.
    std::uint64_t crossfader_transition; ///< Real.
    std::uint64_t main_level_db;         ///< Real.
    std::uint64_t headphone_level_db;    ///< Real.
    std::uint64_t cue_mix;               ///< Real.
    std::uint64_t bus_main;              ///< The main bus's first device channel.
    std::uint64_t bus_cue;               ///< The cue bus's first device channel.
    /// The CRC-32 of the bytes of every word before this one, in memory
    /// order, in the low 32 bits; the high 32 bits are 0.
    std::uint64_t checksum;
};

/// What the engine's last health pass found (specs/health-checks/spec.md
/// requirement 12). A check whose bit isn't in `known` couldn't tell.
struct HealthState {
    /// Health passes so far; 0 before the first.
    std::uint64_t passes;
    /// Host time: when the pass ran.
    std::uint64_t host_time;
    /// kHealthRealtime, kHealthMemory, kHealthPowerProfile, kHealthPowerSource,
    /// kHealthLoad, kHealthAwake: the checks that could tell.
    std::uint64_t known;
    /// The same bits: the checks that fail. Always within `known`.
    std::uint64_t failing;
    /// kPolicy...: the active driver thread's scheduling.
    std::uint64_t policy;
    /// Its priority (Linux), or 0.
    std::uint64_t priority;
    /// RLIMIT_RTPRIO's soft limit (Linux), or 0.
    std::uint64_t rtprio_limit;
    /// RLIMIT_MEMLOCK's soft limit, in bytes; kUnlimited for unlimited.
    std::uint64_t memlock_limit;
    /// kProcessLock...: the last mlockall().
    std::uint64_t process_lock;
    /// Track bytes locked, over the tracks the engine holds.
    std::uint64_t locked_bytes;
    /// Track bytes faulted in whose lock failed.
    std::uint64_t unlocked_bytes;
    /// kPowerProfile...
    std::uint64_t power_profile;
    /// kPowerSource...
    std::uint64_t power_source;
    /// Real: the highest render load in the last 10 s.
    std::uint64_t peak_load;
    /// kAwake... (step 10b).
    std::uint64_t awake;
};

/// The engine-state segment's layout after its header (requirement 7).
/// Zero-filled memory is an EngineState in which nothing has been published,
/// and whose resume slots hold nothing valid.
struct EngineState {
    std::array<Seqlock<DeckState>, kStateDecks> decks;
    Seqlock<MixerState> mixer;
    Seqlock<EngineStatus> status;
    /// Slot i holds the snapshots whose generation modulo 2 is i.
    std::array<Seqlock<ResumeState>, kResumeSlots> resume;
    /// Written by the control thread, after each health pass.
    Seqlock<HealthState> health;
};

/// The layout version in an engine-state segment's header: 2 since the
/// resume slots were added, 3 since the health block was.
inline constexpr std::uint32_t kEngineStateLayoutVersion = 3;

/// An engine-state segment's size: the header, then an EngineState at
/// kSegmentHeaderSize.
inline constexpr std::size_t kEngineStateSegmentSize = kSegmentHeaderSize + sizeof(EngineState);

// The layouts (requirement 8).

static_assert(std::is_standard_layout_v<DeckState> && std::is_trivially_copyable_v<DeckState>);
static_assert(sizeof(DeckState) == 11 * sizeof(std::uint64_t));
static_assert(offsetof(DeckState, heartbeat) == 0);
static_assert(offsetof(DeckState, flags) == 8);
static_assert(offsetof(DeckState, track_id) == 16);
static_assert(offsetof(DeckState, track_sample_rate) == 24);
static_assert(offsetof(DeckState, track_frames) == 32);
static_assert(offsetof(DeckState, frames_ready) == 40);
static_assert(offsetof(DeckState, position) == 48);
static_assert(offsetof(DeckState, rate) == 56);
static_assert(offsetof(DeckState, cue) == 64);
static_assert(offsetof(DeckState, host_time) == 72);
static_assert(offsetof(DeckState, output_latency) == 80);

static_assert(std::is_standard_layout_v<ChannelState> &&
              std::is_trivially_copyable_v<ChannelState>);
static_assert(sizeof(ChannelState) == 4 * sizeof(std::uint64_t));
static_assert(offsetof(ChannelState, trim_db) == 0);
static_assert(offsetof(ChannelState, fader) == 8);
static_assert(offsetof(ChannelState, side) == 16);
static_assert(offsetof(ChannelState, cue) == 24);

static_assert(std::is_standard_layout_v<MixerState> && std::is_trivially_copyable_v<MixerState>);
static_assert(sizeof(MixerState) == (1 + (4 * kStateDecks) + 7) * sizeof(std::uint64_t));
static_assert(offsetof(MixerState, heartbeat) == 0);
static_assert(offsetof(MixerState, channels) == 8);
static_assert(offsetof(MixerState, crossfader) == 136);
static_assert(offsetof(MixerState, crossfader_transition) == 144);
static_assert(offsetof(MixerState, main_level_db) == 152);
static_assert(offsetof(MixerState, headphone_level_db) == 160);
static_assert(offsetof(MixerState, cue_mix) == 168);
static_assert(offsetof(MixerState, bus_main) == 176);
static_assert(offsetof(MixerState, bus_cue) == 184);

static_assert(std::is_standard_layout_v<ClientAck> && std::is_trivially_copyable_v<ClientAck>);
static_assert(sizeof(ClientAck) == 5 * sizeof(std::uint64_t));
static_assert(offsetof(ClientAck, flags) == 0);
static_assert(offsetof(ClientAck, commands_read) == 8);
static_assert(offsetof(ClientAck, last_sequence) == 16);
static_assert(offsetof(ClientAck, rejected) == 24);
static_assert(offsetof(ClientAck, last_rejection) == 32);

static_assert(std::is_standard_layout_v<EngineStatus> &&
              std::is_trivially_copyable_v<EngineStatus>);
static_assert(sizeof(EngineStatus) == (13 + (5 * kStateClients)) * sizeof(std::uint64_t));
static_assert(offsetof(EngineStatus, heartbeat) == 0);
static_assert(offsetof(EngineStatus, frames_rendered) == 8);
static_assert(offsetof(EngineStatus, sample_rate) == 16);
static_assert(offsetof(EngineStatus, host_time) == 24);
static_assert(offsetof(EngineStatus, output_latency) == 32);
static_assert(offsetof(EngineStatus, output_state) == 40);
static_assert(offsetof(EngineStatus, flags) == 48);
static_assert(offsetof(EngineStatus, non_finite_samples) == 56);
static_assert(offsetof(EngineStatus, limited_frames) == 64);
static_assert(offsetof(EngineStatus, late_blocks) == 72);
static_assert(offsetof(EngineStatus, render_time) == 80);
static_assert(offsetof(EngineStatus, render_load) == 88);
static_assert(offsetof(EngineStatus, peak_render_load) == 96);
static_assert(offsetof(EngineStatus, clients) == 104);

static_assert(std::is_standard_layout_v<ResumeDeck> && std::is_trivially_copyable_v<ResumeDeck>);
static_assert(sizeof(ResumeDeck) == 5 * sizeof(std::uint64_t));
static_assert(offsetof(ResumeDeck, track_id) == 0);
static_assert(offsetof(ResumeDeck, flags) == 8);
static_assert(offsetof(ResumeDeck, position) == 16);
static_assert(offsetof(ResumeDeck, rate) == 24);
static_assert(offsetof(ResumeDeck, cue) == 32);

static_assert(std::is_standard_layout_v<ResumeState> && std::is_trivially_copyable_v<ResumeState>);
static_assert(sizeof(ResumeState) ==
              (2 + (5 * kStateDecks) + (4 * kStateDecks) + 8) * sizeof(std::uint64_t));
static_assert(offsetof(ResumeState, generation) == 0);
static_assert(offsetof(ResumeState, host_time) == 8);
static_assert(offsetof(ResumeState, decks) == 16);
static_assert(offsetof(ResumeState, channels) == 176);
static_assert(offsetof(ResumeState, crossfader) == 304);
static_assert(offsetof(ResumeState, crossfader_transition) == 312);
static_assert(offsetof(ResumeState, main_level_db) == 320);
static_assert(offsetof(ResumeState, headphone_level_db) == 328);
static_assert(offsetof(ResumeState, cue_mix) == 336);
static_assert(offsetof(ResumeState, bus_main) == 344);
static_assert(offsetof(ResumeState, bus_cue) == 352);
static_assert(offsetof(ResumeState, checksum) == 360);

static_assert(std::is_standard_layout_v<HealthState> && std::is_trivially_copyable_v<HealthState>);
static_assert(sizeof(HealthState) == 15 * sizeof(std::uint64_t));
static_assert(offsetof(HealthState, passes) == 0);
static_assert(offsetof(HealthState, host_time) == 8);
static_assert(offsetof(HealthState, known) == 16);
static_assert(offsetof(HealthState, failing) == 24);
static_assert(offsetof(HealthState, policy) == 32);
static_assert(offsetof(HealthState, priority) == 40);
static_assert(offsetof(HealthState, rtprio_limit) == 48);
static_assert(offsetof(HealthState, memlock_limit) == 56);
static_assert(offsetof(HealthState, process_lock) == 64);
static_assert(offsetof(HealthState, locked_bytes) == 72);
static_assert(offsetof(HealthState, unlocked_bytes) == 80);
static_assert(offsetof(HealthState, power_profile) == 88);
static_assert(offsetof(HealthState, power_source) == 96);
static_assert(offsetof(HealthState, peak_load) == 104);
static_assert(offsetof(HealthState, awake) == 112);

static_assert(std::is_standard_layout_v<EngineState> && std::is_trivially_copyable_v<EngineState>);
static_assert(offsetof(EngineState, decks) == 0);
static_assert(offsetof(EngineState, mixer) == kStateDecks * sizeof(Seqlock<DeckState>));
static_assert(offsetof(EngineState, status) ==
              offsetof(EngineState, mixer) + sizeof(Seqlock<MixerState>));
static_assert(offsetof(EngineState, resume) ==
              offsetof(EngineState, status) + sizeof(Seqlock<EngineStatus>));
static_assert(offsetof(EngineState, health) ==
              offsetof(EngineState, resume) + (kResumeSlots * sizeof(Seqlock<ResumeState>)));
static_assert(kSegmentHeaderSize % alignof(EngineState) == 0,
              "an EngineState starts right after the segment header");

} // namespace ks::ipc
