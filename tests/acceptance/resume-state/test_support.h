// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Support for the engine tests of specs/resume-state/spec.md: tracks with IDs,
// an engine built either way on a state block and rendered through a render
// core at synthetic host times, copies of a state block as a restarted engine
// finds it, and the snapshot requirement 11 says the engine writes, worked
// out from the engine's own accessors.

#include <ks/engine/block.h>
#include <ks/engine/deck.h>
#include <ks/engine/engine.h>
#include <ks/engine/mixer.h>
#include <ks/engine/render_core.h>
#include <ks/engine/resume.h>
#include <ks/engine/track.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>

#include "../commands/test_support.h"
#include "../state-out/test_support.h"
#include "snapshot_support.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ks::engine::resume_test {

inline constexpr std::uint32_t kRate = 48'000;
inline constexpr std::uint32_t kTrackRate = 44'100;
inline constexpr std::size_t kBlock = 64;
inline constexpr std::size_t kChannels = 4;

/// The host time of engine A's first block.
inline constexpr Clock::time_point kStart{std::chrono::seconds{1'000}};

/// Blocks of 64 frames.
inline constexpr std::array<std::size_t, 1> kSixtyFour{kBlock};

/// A completely decoded stereo track in private memory, with an ID.
class TestTrack {
public:
    template <typename Value>
    TestTrack(std::uint64_t frames, std::uint32_t sample_rate, std::uint64_t id, Value value)
        : samples_(frames * 2), track_{samples_, sample_rate, id} {
        for (std::uint64_t frame = 0; frame < frames; ++frame) {
            for (std::size_t channel = 0; channel < 2; ++channel) {
                samples_.at((frame * 2) + channel) = value(frame, channel);
            }
        }
        track_.publish(frames, true);
    }

    TestTrack(const TestTrack&) = delete;
    TestTrack& operator=(const TestTrack&) = delete;
    TestTrack(TestTrack&&) = delete;
    TestTrack& operator=(TestTrack&&) = delete;
    ~TestTrack() = default;

    [[nodiscard]] Track& track() noexcept { return track_; }
    [[nodiscard]] const Track* get() const noexcept { return &track_; }

private:
    std::vector<float> samples_;
    Track track_;
};

/// The deck tests' synthetic music, `offset` frames in, at `sample_rate`.
inline std::unique_ptr<TestTrack> music(std::uint64_t id, std::uint32_t sample_rate = kRate,
                                        std::uint64_t frames = 480'000, std::uint64_t offset = 0) {
    return std::make_unique<TestTrack>(
        frames, sample_rate, id, [offset, sample_rate](std::uint64_t frame, std::size_t channel) {
            return test::synthetic(frame + offset, channel, sample_rate);
        });
}

/// `value` on the left and its negative on the right, at 48 kHz.
inline std::unique_ptr<TestTrack> constant(std::uint64_t id, float value,
                                           std::uint64_t frames = 300'000) {
    return std::make_unique<TestTrack>(frames, kRate, id,
                                       [value](std::uint64_t /*frame*/, std::size_t channel) {
                                           return channel == 0 ? value : -value;
                                       });
}

/// A zero-filled state block on the heap.
inline std::unique_ptr<ipc::EngineState> fresh_state() {
    return std::make_unique<ipc::EngineState>();
}

/// A copy of `state`, as a restarted engine would find it. Single-threaded.
inline std::unique_ptr<ipc::EngineState> copy_of(const ipc::EngineState& state) {
    auto copy = std::make_unique<ipc::EngineState>();
    *copy = state;
    return copy;
}

/// An engine on a state block, rendered by a render core in blocks at
/// consecutive host times. Engine A is built with the state-block
/// constructor; engine B, on a copy of A's block, with the resuming one.
class Rig {
public:
    /// Builds the engine with Engine(state, map).
    explicit Rig(std::unique_ptr<ipc::EngineState> state, const BusMap& map = {},
                 std::uint32_t sample_rate = kRate)
        : state_{std::move(state)}, engine_{std::make_unique<Engine>(*state_, map)},
          core_{std::make_unique<RenderCore>(*engine_)}, sample_rate_{sample_rate} {}

    /// Builds the engine with Engine(state, tracks, map).
    Rig(std::unique_ptr<ipc::EngineState> state, std::span<const Track* const> tracks,
        const BusMap& map = {}, std::uint32_t sample_rate = kRate)
        : state_{std::move(state)}, engine_{std::make_unique<Engine>(*state_, tracks, map)},
          core_{std::make_unique<RenderCore>(*engine_)}, sample_rate_{sample_rate} {}

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    Rig(Rig&&) = delete;
    Rig& operator=(Rig&&) = delete;
    ~Rig() = default;

    /// Renders a block of `frames` frames at the next host time, which moves
    /// on if it rendered.
    RenderResult render(std::size_t frames = kBlock) {
        const Block block{.channels = channels_.views(),
                          .frames = frames,
                          .sample_rate = sample_rate_,
                          .host_time = next_,
                          .output_latency = {},
                          .resumed = false};
        const RenderResult result = core_->try_render(block);
        if (result == RenderResult::Rendered) {
            next_ += frames_to_duration(frames, sample_rate_);
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

    /// Renders `frames` frames in blocks whose sizes cycle through `sizes`,
    /// and returns every channel's samples.
    test::Outputs render_frames(std::size_t frames,
                                std::span<const std::size_t> sizes = kSixtyFour) {
        test::Outputs out(kChannels);
        for (std::vector<float>& channel : out) {
            channel.reserve(frames);
        }
        std::size_t next_size = 0;
        for (std::size_t done = 0; done < frames;) {
            const std::size_t n = std::min(sizes[next_size % sizes.size()], frames - done);
            ++next_size;
            static_cast<void>(render(n));
            for (std::size_t c = 0; c < kChannels; ++c) {
                for (std::size_t f = 0; f < n; ++f) {
                    out.at(c).push_back(channels_.at(c, f));
                }
            }
            done += n;
        }
        return out;
    }

    /// The host time at which the next block starts.
    [[nodiscard]] Clock::time_point next_time() const { return next_; }
    void set_next_time(Clock::time_point time) { next_ = time; }

    /// Blocks rendered.
    [[nodiscard]] std::uint64_t blocks() const { return blocks_; }

    [[nodiscard]] ipc::EngineState& state() { return *state_; }
    [[nodiscard]] Engine& engine() { return *engine_; }
    [[nodiscard]] const Deck& deck(std::size_t i) const { return engine_->mixer().deck(i); }
    [[nodiscard]] const MixerSettings& settings() const { return engine_->mixer().settings(); }

private:
    std::unique_ptr<ipc::EngineState> state_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<RenderCore> core_;
    std::uint32_t sample_rate_;
    state_test::Channels channels_;
    Clock::time_point next_ = kStart;
    std::uint64_t blocks_ = 0;
};

/// The snapshot requirement 11 says `engine` writes as generation
/// `generation` after a block ending at `end`, from its decks' and mixer's
/// accessors. A deck without a track, or whose track has ID 0 (which no
/// engine could resume), is written as empty, so every snapshot is valid.
inline Snapshot expected_snapshot(const Engine& engine, std::uint64_t generation,
                                  Clock::time_point end) {
    Snapshot snapshot;
    snapshot.generation = generation;
    snapshot.host_time = end;
    for (std::size_t i = 0; i < kDecks; ++i) {
        const Deck& deck = engine.mixer().deck(i);
        DeckSnapshot& out = snapshot.decks.at(i);
        out.rate = deck.rate();
        const Track* const track = deck.track();
        if (track != nullptr && track->id() != 0) {
            out.track_id = track->id();
            out.playing = deck.playing();
            out.position = deck.position();
            out.cue = deck.cue();
        }
    }
    snapshot.mixer = engine.mixer().settings();
    return snapshot;
}

/// Resume slot `slot`'s words as a SeqlockReader reads them; nothing if the
/// slot is mid-write.
inline std::optional<ipc::ResumeState> read_slot(ipc::EngineState& state, std::size_t slot) {
    return ipc::SeqlockReader<ipc::ResumeState>{state.resume.at(slot)}.try_read();
}

/// The snapshot in resume slot `slot`, if it's valid.
inline std::optional<Snapshot> slot_snapshot(ipc::EngineState& state, std::size_t slot) {
    const std::optional<ipc::ResumeState> words = read_slot(state, slot);
    if (!words.has_value()) {
        return std::nullopt;
    }
    return validate(*words, slot);
}

/// The generation in slot `slot`, if the slot is valid; 0 if not.
inline std::uint64_t slot_generation(ipc::EngineState& state, std::size_t slot) {
    return slot_snapshot(state, slot).value_or(Snapshot{}).generation;
}

/// Flips one bit of one word in slot `slot`. Single-threaded.
inline void flip_bit(ipc::EngineState& state, std::size_t slot, std::size_t word, unsigned bit) {
    Words words = words_of(slot_words(state.resume.at(slot)));
    words.at(word) ^= std::uint64_t{1} << bit;
    set_slot_words(state.resume.at(slot), std::bit_cast<ipc::ResumeState>(words));
}

/// Changes both slots' words and corrects their checksums, so only the range
/// checks can catch the change. Single-threaded.
inline void forge(ipc::EngineState& state, const std::function<void(ipc::ResumeState&)>& change) {
    for (ipc::Seqlock<ipc::ResumeState>& lock : state.resume) {
        ipc::ResumeState words = slot_words(lock);
        change(words);
        set_slot_words(lock, with_checksum(words));
    }
}

} // namespace ks::engine::resume_test
