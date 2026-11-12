// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the engine acceptance tests: clients that send commands, command
// builders, calling controls directly for comparison, rendering an engine,
// and a ledger of track references. Builds on the codec support and the mixer
// and deck tests' support.

#include <ks/engine/block.h>
#include <ks/engine/command.h>
#include <ks/engine/engine.h>
#include <ks/engine/mixer.h>
#include <ks/engine/track.h>
#include <ks/ipc/command.h>
#include <ks/ipc/spsc_ring.h>

#include "../mixer/test_support.h"
#include "codec_support.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ks::engine::test {

using CommandProducer = ipc::RingProducer<ipc::CommandSlot, ipc::kCommandRingCapacity>;

/// A client: its command ring, on the heap, and the producer that writes it.
class Client {
public:
    Client() : ring_{std::make_unique<ipc::CommandRing>()}, producer_{*ring_} {}

    [[nodiscard]] ipc::CommandRing& ring() { return *ring_; }

    /// Encodes `command` with the next sequence number, starting from 1, and
    /// pushes it. Returns whether it fit.
    bool send(const Command& command) { return producer_.try_push(encode(++sequence_, command)); }

    /// Pushes a slot as it is. Returns whether it fit.
    bool send_slot(const ipc::CommandSlot& slot) { return producer_.try_push(slot); }

    /// The last sequence number send() used.
    [[nodiscard]] std::uint64_t sequence() const { return sequence_; }

private:
    std::unique_ptr<ipc::CommandRing> ring_;
    CommandProducer producer_;
    std::uint64_t sequence_ = 0;
};

// Command builders.

inline Command playing(std::size_t deck, bool on) {
    return Command{.type = ipc::CommandType::SetPlaying, .deck = deck, .on = on};
}

/// A deck or channel command that carries a double.
inline Command deck_command(ipc::CommandType type, std::size_t deck, double value) {
    return Command{.type = type, .deck = deck, .value = value};
}

/// A global command that carries a double.
inline Command global_command(ipc::CommandType type, double value) {
    return Command{.type = type, .value = value};
}

inline Command assign(std::size_t deck, CrossfaderSide side) {
    return Command{.type = ipc::CommandType::SetCrossfaderAssign, .deck = deck, .side = side};
}

inline Command channel_cue(std::size_t deck, bool on) {
    return Command{.type = ipc::CommandType::SetChannelCue, .deck = deck, .on = on};
}

inline Command bus_map(BusMap map) {
    return Command{.type = ipc::CommandType::SetBusMap, .bus_map = map};
}

/// Calls the control `command` names directly, as requirement 5 says apply()
/// does, but without using apply().
inline void call_directly(Mixer& mixer, const Command& command) {
    Deck& deck = mixer.deck(std::min(command.deck, kDecks - 1));
    using enum ipc::CommandType;
    switch (command.type) {
    case SetPlaying:
        deck.set_playing(command.on);
        return;
    case SetRate:
        deck.set_rate(command.value);
        return;
    case Seek:
        deck.seek(command.value);
        return;
    case SetCuePoint:
        deck.set_cue(command.value);
        return;
    case SetTrim:
        mixer.set_trim(command.deck, command.value);
        return;
    case SetFader:
        mixer.set_fader(command.deck, command.value);
        return;
    case SetCrossfaderAssign:
        mixer.set_crossfader_assign(command.deck, command.side);
        return;
    case SetChannelCue:
        mixer.set_cue(command.deck, command.on);
        return;
    case SetCrossfader:
        mixer.set_crossfader(command.value);
        return;
    case SetCrossfaderCurve:
        mixer.set_crossfader_curve(command.value);
        return;
    case SetMainLevel:
        mixer.set_main_level(command.value);
        return;
    case SetHeadphoneLevel:
        mixer.set_headphone_level(command.value);
        return;
    case SetCueMix:
        mixer.set_cue_mix(command.value);
        return;
    case SetBusMap:
        mixer.set_bus_map(command.bus_map);
        return;
    }
}

/// Renders one block of `frames` frames into `out`, starting at frame `at`.
inline void process_into(Graph& graph, Outputs& out, std::size_t at, std::size_t frames) {
    std::vector<std::span<float>> views(out.size());
    for (std::size_t c = 0; c < out.size(); ++c) {
        views.at(c) = std::span{out.at(c)}.subspan(at, frames);
    }
    graph.process(Block{.channels = views,
                        .frames = frames,
                        .sample_rate = kRate,
                        .host_time = {},
                        .resumed = false});
}

/// Renders `frames` frames of `channels` zero-filled channels through
/// `graph`, in blocks of `block_frames`.
inline Outputs render(Graph& graph, std::size_t frames, std::size_t channels = 4,
                      std::size_t block_frames = 64) {
    Outputs out(channels, std::vector<float>(frames, 0.0F));
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min(block_frames, frames - done);
        process_into(graph, out, done, n);
        done += n;
    }
    return out;
}

/// The references given to an engine and the releases collected from it.
class Ledger {
public:
    /// Records a successful load().
    void loaded(const Track& track) {
        ++outstanding_[&track];
        ++loads_;
    }

    /// Records a successful detach().
    void detached(const ipc::CommandRing& ring) { ++detaches_[&ring]; }

    /// Collects every release waiting in `engine`. Returns false if one had
    /// no outstanding reference or detach to match it, or had neither or
    /// both pointers set.
    bool collect_all(Engine& engine) {
        bool ok = true;
        while (const std::optional<Released> released = engine.collect()) {
            ++releases_;
            const bool has_track = released->track != nullptr;
            const bool has_ring = released->ring != nullptr;
            if (has_track == has_ring) {
                ok = false;
            } else if (has_track) {
                std::size_t& count = outstanding_[released->track];
                if (count == 0) {
                    ok = false;
                } else {
                    --count;
                    ++track_releases_;
                }
            } else {
                std::size_t& count = detaches_[released->ring];
                if (count == 0) {
                    ok = false;
                } else {
                    --count;
                    rings_released_.push_back(released->ring);
                }
            }
        }
        return ok;
    }

    /// References to `track` not yet released.
    [[nodiscard]] std::size_t outstanding(const Track& track) const {
        const auto found = outstanding_.find(&track);
        return found == outstanding_.end() ? 0 : found->second;
    }

    /// References to any track not yet released.
    [[nodiscard]] std::size_t outstanding() const {
        std::size_t total = 0;
        for (const auto& [track, count] : outstanding_) {
            total += count;
        }
        return total;
    }

    /// Detaches whose rings haven't been released yet.
    [[nodiscard]] std::size_t detaches_outstanding() const {
        std::size_t total = 0;
        for (const auto& [ring, count] : detaches_) {
            total += count;
        }
        return total;
    }

    [[nodiscard]] std::size_t loads() const { return loads_; }
    [[nodiscard]] std::size_t track_releases() const { return track_releases_; }
    [[nodiscard]] std::size_t releases() const { return releases_; }

    /// Rings released, in the order they were collected.
    [[nodiscard]] const std::vector<ipc::CommandRing*>& rings_released() const {
        return rings_released_;
    }

private:
    std::map<const Track*, std::size_t> outstanding_;
    std::map<const ipc::CommandRing*, std::size_t> detaches_;
    std::vector<ipc::CommandRing*> rings_released_;
    std::size_t loads_ = 0;
    std::size_t track_releases_ = 0;
    std::size_t releases_ = 0;
};

/// A constant 48 kHz track: `value` on the left, its negative on the right.
inline std::unique_ptr<OwnedTrack> constant_track(float value, std::uint64_t frames = 100'000) {
    return std::make_unique<OwnedTrack>(frames, kRate,
                                        [value](std::uint64_t /*frame*/, std::size_t channel) {
                                            return channel == 0 ? value : -value;
                                        });
}

/// The synthetic music track from the deck tests, at 44.1 kHz, starting
/// `offset` frames in.
inline std::unique_ptr<OwnedTrack> music_track(std::uint64_t offset,
                                               std::uint64_t frames = 100'000) {
    return std::make_unique<OwnedTrack>(frames, kTrackRate,
                                        [offset](std::uint64_t frame, std::size_t channel) {
                                            return synthetic(frame + offset, channel, kTrackRate);
                                        });
}

} // namespace ks::engine::test
