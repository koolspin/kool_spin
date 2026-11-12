// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Support for the acceptance tests of specs/state-out/spec.md: tracks with
// IDs, rendering an engine through a render core at synthetic host times, and
// what each state block should hold, worked out from the spec independently
// of the engine.

#include <ks/core/realtime.h>
#include <ks/engine/block.h>
#include <ks/engine/command.h>
#include <ks/engine/engine.h>
#include <ks/engine/mixer.h>
#include <ks/engine/render_core.h>
#include <ks/engine/track.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace ks::engine::state_test {

inline constexpr std::uint32_t kRate = 48'000;
inline constexpr std::size_t kBlock = 64;
inline constexpr std::size_t kChannels = 4;

using Clock = std::chrono::steady_clock;

/// The host time of the first block the tests render.
inline constexpr Clock::time_point kStart{std::chrono::seconds{1'000}};

/// True if every byte of `value` is zero.
template <typename T>
bool all_zero(const T& value) {
    const auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    return std::ranges::all_of(bytes, [](std::byte b) { return b == std::byte{0}; });
}

/// The double whose bit pattern is `word`.
inline double real(std::uint64_t word) {
    return std::bit_cast<double>(word);
}

/// The bit pattern of `value`.
inline std::uint64_t bits(double value) {
    return std::bit_cast<std::uint64_t>(value);
}

/// A host time as the layout stores it.
inline std::uint64_t time_word(Clock::time_point time) {
    const std::int64_t ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    return std::bit_cast<std::uint64_t>(ns);
}

/// A duration as the layout stores it.
inline std::uint64_t duration_word(std::chrono::nanoseconds duration) {
    return static_cast<std::uint64_t>(duration.count());
}

/// A stereo track in private memory with an ID: `value` on the left and its
/// negative on the right. Unless told otherwise, every frame is published as
/// ready and complete.
class IdTrack {
public:
    IdTrack(std::uint64_t frames, std::uint64_t id, float value = 0.25F,
            std::uint32_t sample_rate = kRate, bool publish = true)
        : samples_(frames * 2, value), track_{samples_, sample_rate, id} {
        for (std::size_t i = 1; i < samples_.size(); i += 2) {
            samples_.at(i) = -value;
        }
        if (publish) {
            track_.publish(frames, true);
        }
    }

    IdTrack(const IdTrack&) = delete;
    IdTrack& operator=(const IdTrack&) = delete;
    IdTrack(IdTrack&&) = delete;
    IdTrack& operator=(IdTrack&&) = delete;
    ~IdTrack() = default;

    [[nodiscard]] Track& track() noexcept { return track_; }

private:
    std::vector<float> samples_;
    Track track_;
};

/// Planar output buffers, one per channel, each kMaxBlockFrames long.
class Channels {
public:
    Channels() {
        for (std::size_t c = 0; c < kChannels; ++c) {
            views_.at(c) = std::span{storage_.at(c)};
        }
    }

    Channels(const Channels&) = delete;
    Channels& operator=(const Channels&) = delete;
    Channels(Channels&&) = delete;
    Channels& operator=(Channels&&) = delete;
    ~Channels() = default;

    [[nodiscard]] std::span<const std::span<float>> views() const { return views_; }

    /// Sample `frame` of channel `channel`.
    [[nodiscard]] float at(std::size_t channel, std::size_t frame) const {
        return storage_.at(channel).at(frame);
    }

private:
    std::array<std::vector<float>, kChannels> storage_{
        std::vector<float>(kMaxBlockFrames), std::vector<float>(kMaxBlockFrames),
        std::vector<float>(kMaxBlockFrames), std::vector<float>(kMaxBlockFrames)};
    std::array<std::span<float>, kChannels> views_{};
};

/// An engine that publishes to a state block, run by a render core, with
/// blocks at consecutive synthetic host times from kStart.
class EngineRig {
public:
    explicit EngineRig(bool with_state = true)
        : state_{std::make_unique<ipc::EngineState>()},
          engine_{with_state ? std::make_unique<Engine>(*state_) : std::make_unique<Engine>()},
          core_{*engine_} {}

    EngineRig(const EngineRig&) = delete;
    EngineRig& operator=(const EngineRig&) = delete;
    EngineRig(EngineRig&&) = delete;
    EngineRig& operator=(EngineRig&&) = delete;
    ~EngineRig() = default;

    /// Renders a block of `frames` frames as `driver`, at the next host time.
    /// The host time moves on only if the block rendered.
    RenderResult render(std::size_t frames = kBlock, DriverId driver = kDefaultDriver) {
        const Block block{.channels = channels_.views(),
                          .frames = frames,
                          .sample_rate = kRate,
                          .host_time = next_,
                          .output_latency = latency_,
                          .resumed = false};
        const RenderResult result = core_.try_render(block, driver);
        if (result == RenderResult::Rendered) {
            next_ += frames_to_duration(frames, kRate);
            ++blocks_;
        }
        return result;
    }

    /// Renders `count` blocks of 64 frames.
    void render_blocks(std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            static_cast<void>(render());
        }
    }

    void set_latency(std::chrono::nanoseconds latency) { latency_ = latency; }
    [[nodiscard]] std::chrono::nanoseconds latency() const { return latency_; }

    /// The host time at which the next block starts: the end of the last.
    [[nodiscard]] Clock::time_point next_time() const { return next_; }

    /// Blocks rendered so far, counted by the rig.
    [[nodiscard]] std::uint64_t blocks() const { return blocks_; }

    [[nodiscard]] ipc::EngineState& state() { return *state_; }
    [[nodiscard]] Engine& engine() { return *engine_; }
    [[nodiscard]] RenderCore& core() { return core_; }
    [[nodiscard]] const Channels& output() const { return channels_; }

private:
    std::unique_ptr<ipc::EngineState> state_;
    std::unique_ptr<Engine> engine_;
    RenderCore core_;
    Channels channels_;
    Clock::time_point next_ = kStart;
    std::chrono::nanoseconds latency_{};
    std::uint64_t blocks_ = 0;
};

/// A consistent copy of a seqlock's value. Single-threaded tests never fail
/// to read; a failure reads as all zeros.
template <typename T>
T read(ipc::Seqlock<T>& lock) {
    return ipc::SeqlockReader<T>{lock}.try_read().value_or(T{});
}

/// A seqlock's sequence number.
template <typename T>
std::uint64_t sequence(ipc::Seqlock<T>& lock) {
    return ipc::load_shared(lock.sequence, std::memory_order_acquire);
}

// What each block should hold (requirements 3 to 6), from the engine's and
// the render core's own accessors.

/// Deck `deck`'s block after a block that ended at `end`.
inline ipc::DeckState expected_deck(const Deck& deck, std::uint64_t heartbeat,
                                    Clock::time_point end, std::chrono::nanoseconds latency) {
    ipc::DeckState expected{};
    expected.heartbeat = heartbeat;
    const Track* const track = deck.track();
    if (track != nullptr) {
        const TrackReady ready = track->ready();
        expected.flags |= ipc::kDeckLoaded;
        expected.flags |= ready.complete ? ipc::kDeckComplete : 0;
        expected.track_id = track->id();
        expected.track_sample_rate = track->sample_rate();
        expected.track_frames = track->capacity();
        expected.frames_ready = ready.frames;
    }
    expected.flags |= deck.playing() ? ipc::kDeckPlaying : 0;
    expected.flags |= deck.waiting() ? ipc::kDeckWaiting : 0;
    expected.position = bits(deck.position());
    expected.rate = bits(deck.rate());
    expected.cue = bits(deck.cue());
    expected.host_time = time_word(end);
    expected.output_latency = duration_word(latency);
    return expected;
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

/// The mixer's block.
inline ipc::MixerState expected_mixer(const MixerSettings& settings, std::uint64_t heartbeat) {
    ipc::MixerState expected{};
    expected.heartbeat = heartbeat;
    for (std::size_t i = 0; i < kDecks; ++i) {
        const ChannelSettings& channel = settings.channels.at(i);
        ipc::ChannelState& state = expected.channels.at(i);
        state.trim_db = bits(channel.trim_db);
        state.fader = bits(channel.fader);
        state.side = side_code(channel.side);
        state.cue = channel.cue ? 1 : 0;
    }
    expected.crossfader = bits(settings.crossfader);
    expected.crossfader_transition = bits(settings.crossfader_transition);
    expected.main_level_db = bits(settings.main_level_db);
    expected.headphone_level_db = bits(settings.headphone_level_db);
    expected.cue_mix = bits(settings.cue_mix);
    expected.bus_main = settings.bus_map.main;
    expected.bus_cue = settings.bus_map.cue;
    return expected;
}

inline std::uint64_t rejection_code(const std::optional<Rejection>& rejection) {
    if (!rejection.has_value()) {
        return 0;
    }
    switch (*rejection) {
    case Rejection::UnknownType:
        return 1;
    case Rejection::BadTarget:
        return 2;
    case Rejection::NonzeroPadding:
        return 3;
    case Rejection::BadValue:
        return 4;
    }
    return 99;
}

/// One client slot's acknowledgement.
inline ipc::ClientAck expected_ack(const ClientStats& stats) {
    ipc::ClientAck expected{};
    expected.flags =
        (stats.attached ? ipc::kClientAttached : 0) | (stats.corrupt ? ipc::kClientCorrupt : 0);
    expected.commands_read = stats.commands_read;
    expected.last_sequence = stats.last_sequence;
    expected.rejected = stats.rejected;
    expected.last_rejection = rejection_code(stats.last_rejection);
    return expected;
}

inline std::uint64_t output_state_code(OutputState state) {
    switch (state) {
    case OutputState::Stopped:
        return 0;
    case OutputState::Playing:
        return 1;
    case OutputState::Freewheeling:
        return 2;
    case OutputState::Attaching:
        return 3;
    }
    return 99;
}

/// The status block after the rig's last block, from the render core's
/// accessors and the engine's client statistics.
inline ipc::EngineStatus expected_status(EngineRig& rig) {
    const RenderCore& core = rig.core();
    ipc::EngineStatus expected{};
    expected.heartbeat = core.blocks_rendered();
    expected.frames_rendered = core.frames_rendered();
    expected.sample_rate = kRate;
    expected.host_time = time_word(core.rendered_until());
    expected.output_latency = duration_word(rig.latency());
    expected.output_state = output_state_code(core.output_state());
    expected.flags =
        (core.muted() ? ipc::kStatusMuted : 0) | (core.silenced() ? ipc::kStatusSilenced : 0);
    expected.non_finite_samples = core.non_finite_samples();
    expected.limited_frames = core.limited_frames();
    expected.late_blocks = core.late_blocks();
    expected.render_time = duration_word(core.render_time());
    expected.render_load = bits(core.render_load());
    expected.peak_render_load = bits(core.peak_render_load());
    for (std::size_t i = 0; i < kMaxClients; ++i) {
        expected.clients.at(i) = expected_ack(rig.engine().client_stats(i));
    }
    return expected;
}

// Differences between two blocks, field by field, as text: empty if they're
// equal. Reals are compared as bit patterns.

class Diff {
public:
    void field(const char* name, std::uint64_t actual, std::uint64_t expected) {
        if (actual != expected) {
            out_ << name << ": " << actual << " (0x" << std::hex << actual << std::dec
                 << "), expected " << expected << " (0x" << std::hex << expected << std::dec
                 << "); ";
        }
    }

    [[nodiscard]] std::string str() const { return out_.str(); }

private:
    std::ostringstream out_;
};

inline std::string differences(const ipc::DeckState& a, const ipc::DeckState& e) {
    Diff d;
    d.field("heartbeat", a.heartbeat, e.heartbeat);
    d.field("flags", a.flags, e.flags);
    d.field("track_id", a.track_id, e.track_id);
    d.field("track_sample_rate", a.track_sample_rate, e.track_sample_rate);
    d.field("track_frames", a.track_frames, e.track_frames);
    d.field("frames_ready", a.frames_ready, e.frames_ready);
    d.field("position", a.position, e.position);
    d.field("rate", a.rate, e.rate);
    d.field("cue", a.cue, e.cue);
    d.field("host_time", a.host_time, e.host_time);
    d.field("output_latency", a.output_latency, e.output_latency);
    return d.str();
}

inline std::string differences(const ipc::MixerState& a, const ipc::MixerState& e) {
    Diff d;
    d.field("heartbeat", a.heartbeat, e.heartbeat);
    for (std::size_t i = 0; i < kDecks; ++i) {
        const ipc::ChannelState& ac = a.channels.at(i);
        const ipc::ChannelState& ec = e.channels.at(i);
        d.field("trim_db", ac.trim_db, ec.trim_db);
        d.field("fader", ac.fader, ec.fader);
        d.field("side", ac.side, ec.side);
        d.field("cue", ac.cue, ec.cue);
    }
    d.field("crossfader", a.crossfader, e.crossfader);
    d.field("crossfader_transition", a.crossfader_transition, e.crossfader_transition);
    d.field("main_level_db", a.main_level_db, e.main_level_db);
    d.field("headphone_level_db", a.headphone_level_db, e.headphone_level_db);
    d.field("cue_mix", a.cue_mix, e.cue_mix);
    d.field("bus_main", a.bus_main, e.bus_main);
    d.field("bus_cue", a.bus_cue, e.bus_cue);
    return d.str();
}

inline std::string differences(const ipc::ClientAck& a, const ipc::ClientAck& e) {
    Diff d;
    d.field("flags", a.flags, e.flags);
    d.field("commands_read", a.commands_read, e.commands_read);
    d.field("last_sequence", a.last_sequence, e.last_sequence);
    d.field("rejected", a.rejected, e.rejected);
    d.field("last_rejection", a.last_rejection, e.last_rejection);
    return d.str();
}

inline std::string differences(const ipc::EngineStatus& a, const ipc::EngineStatus& e) {
    Diff d;
    d.field("heartbeat", a.heartbeat, e.heartbeat);
    d.field("frames_rendered", a.frames_rendered, e.frames_rendered);
    d.field("sample_rate", a.sample_rate, e.sample_rate);
    d.field("host_time", a.host_time, e.host_time);
    d.field("output_latency", a.output_latency, e.output_latency);
    d.field("output_state", a.output_state, e.output_state);
    d.field("flags", a.flags, e.flags);
    d.field("non_finite_samples", a.non_finite_samples, e.non_finite_samples);
    d.field("limited_frames", a.limited_frames, e.limited_frames);
    d.field("late_blocks", a.late_blocks, e.late_blocks);
    d.field("render_time", a.render_time, e.render_time);
    d.field("render_load", a.render_load, e.render_load);
    d.field("peak_render_load", a.peak_render_load, e.peak_render_load);
    std::string out = d.str();
    for (std::size_t i = 0; i < kMaxClients; ++i) {
        const std::string client = differences(a.clients.at(i), e.clients.at(i));
        if (!client.empty()) {
            out += "client " + std::to_string(i) + ": " + client;
        }
    }
    return out;
}

/// Differences between every block in `state` and what the rig's engine and
/// render core hold after its last block.
inline std::string differences(EngineRig& rig) {
    ipc::EngineState& state = rig.state();
    const std::uint64_t heartbeat = rig.core().blocks_rendered();
    std::string out;
    for (std::size_t i = 0; i < kDecks; ++i) {
        const std::string deck = differences(
            read(state.decks.at(i)),
            expected_deck(rig.engine().mixer().deck(i), heartbeat, rig.next_time(), rig.latency()));
        if (!deck.empty()) {
            out += "deck " + std::to_string(i) + ": " + deck;
        }
    }
    const std::string mixer =
        differences(read(state.mixer), expected_mixer(rig.engine().mixer().settings(), heartbeat));
    if (!mixer.empty()) {
        out += "mixer: " + mixer;
    }
    const std::string status = differences(read(state.status), expected_status(rig));
    if (!status.empty()) {
        out += "status: " + status;
    }
    return out;
}

} // namespace ks::engine::state_test
