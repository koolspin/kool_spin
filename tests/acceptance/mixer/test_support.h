// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the mixer acceptance tests: a mixer whose decks play constants,
// rendering a mixer in blocks, and scripts of controls. Builds on the deck
// tests' support (tracks, kRate, the synthetic track).

#include <ks/core/realtime.h>
#include <ks/engine/block.h>
#include <ks/engine/deck.h>
#include <ks/engine/interpolator.h>
#include <ks/engine/mixer.h>
#include <ks/engine/render_core.h>

#include "../deck/test_support.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace ks::engine::test {

/// The output of a mixer: one vector per channel.
using Outputs = std::vector<std::vector<float>>;

/// Deck i's constant on the left channel; the right is its negative.
inline float deck_value(std::size_t deck) {
    return 0.05F * static_cast<float>(deck + 1);
}

/// A mixer whose decks are loaded with constant 48 kHz tracks: deck i plays
/// deck_value(i) on the left and its negative on the right.
struct Rig {
    explicit Rig(const BusMap& map = {}) : mixer{interpolator, map} {
        for (std::size_t i = 0; i < kDecks; ++i) {
            const float value = deck_value(i);
            tracks.push_back(std::make_unique<OwnedTrack>(
                100'000, kRate, [value](std::uint64_t /*frame*/, std::size_t channel) {
                    return channel == 0 ? value : -value;
                }));
            static_cast<void>(mixer.deck(i).load(tracks.back()->track()));
        }
    }

    void play(std::size_t deck) { mixer.deck(deck).set_playing(true); }

    void play_all() {
        for (std::size_t i = 0; i < kDecks; ++i) {
            play(i);
        }
    }

    Interpolator interpolator;
    std::vector<std::unique_ptr<OwnedTrack>> tracks;
    Mixer mixer;
};

/// Renders `frames` frames of `channels` zero-filled channels through
/// `mixer`, calling process() directly in blocks of `block_frames`.
inline Outputs render(Mixer& mixer, std::size_t frames, std::size_t channels = 4,
                      std::size_t block_frames = 64, std::uint32_t sample_rate = kRate) {
    Outputs out(channels, std::vector<float>(frames, 0.0F));
    std::vector<std::span<float>> views(channels);
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min(block_frames, frames - done);
        for (std::size_t c = 0; c < channels; ++c) {
            views.at(c) = std::span{out.at(c)}.subspan(done, n);
        }
        mixer.process(Block{.channels = views,
                            .frames = n,
                            .sample_rate = sample_rate,
                            .host_time = {},
                            .resumed = false});
        done += n;
    }
    return out;
}

/// The last frame of channel `channel`.
inline float last(const Outputs& out, std::size_t channel) {
    return out.at(channel).back();
}

/// Side A's crossfader gain (spec requirement 4).
inline double side_a(double position, double transition) {
    if (position <= 1.0 - transition) {
        return 1.0;
    }
    return std::cos(std::numbers::pi / 2.0 * (position - (1.0 - transition)) / transition);
}

/// Side B's crossfader gain.
inline double side_b(double position, double transition) {
    return side_a(1.0 - position, transition);
}

/// 10^(db / 20).
inline double gain_of_db(double db) {
    return std::pow(10.0, db / 20.0);
}

// Scripts of controls, for the block-size test and the listening test.

enum class MixerAction : std::uint8_t {
    Play,
    Pause,
    Seek,
    Rate,
    Trim,
    Fader,
    Side,
    Cue,
    Crossfader,
    Curve,
    MainLevel,
    HeadphoneLevel,
    CueMix,
};

/// A control applied before frame `frame`. `deck` is used by the per-deck
/// actions; `value` is the value, the side (0 is A, 1 is B, 2 is thru), or,
/// for Cue, nonzero for on.
struct MixerControl {
    std::uint64_t frame = 0;
    MixerAction action = MixerAction::Play;
    std::size_t deck = 0;
    double value = 0.0;
};

/// A Side control's value as a side: 0 is A, 1 is B, anything else thru.
inline CrossfaderSide side_of(double value) noexcept KS_NONBLOCKING {
    if (value == 0.0) {
        return CrossfaderSide::A;
    }
    if (value == 1.0) {
        return CrossfaderSide::B;
    }
    return CrossfaderSide::Thru;
}

inline void apply(Mixer& mixer, const MixerControl& control) noexcept KS_NONBLOCKING {
    Deck& deck = mixer.deck(std::min(control.deck, kDecks - 1));
    switch (control.action) {
    case MixerAction::Play:
        deck.set_playing(true);
        return;
    case MixerAction::Pause:
        deck.set_playing(false);
        return;
    case MixerAction::Seek:
        deck.seek(control.value);
        return;
    case MixerAction::Rate:
        deck.set_rate(control.value);
        return;
    case MixerAction::Trim:
        mixer.set_trim(control.deck, control.value);
        return;
    case MixerAction::Fader:
        mixer.set_fader(control.deck, control.value);
        return;
    case MixerAction::Side:
        mixer.set_crossfader_assign(control.deck, side_of(control.value));
        return;
    case MixerAction::Cue:
        mixer.set_cue(control.deck, control.value != 0.0);
        return;
    case MixerAction::Crossfader:
        mixer.set_crossfader(control.value);
        return;
    case MixerAction::Curve:
        mixer.set_crossfader_curve(control.value);
        return;
    case MixerAction::MainLevel:
        mixer.set_main_level(control.value);
        return;
    case MixerAction::HeadphoneLevel:
        mixer.set_headphone_level(control.value);
        return;
    case MixerAction::CueMix:
        mixer.set_cue_mix(control.value);
        return;
    }
}

/// Renders `frames` frames of `script` through `mixer` on 4 channels, in
/// blocks of at most `block_frames`, split so each control applies at its
/// frame. The script must be in frame order.
inline Outputs perform(Mixer& mixer, std::span<const MixerControl> script, std::uint64_t frames,
                       std::size_t block_frames) {
    Outputs out(4, std::vector<float>(frames, 0.0F));
    std::vector<std::span<float>> views(4);
    std::size_t next = 0;
    for (std::uint64_t frame = 0; frame < frames;) {
        while (next < script.size() && script[next].frame <= frame) {
            apply(mixer, script[next++]);
        }
        const std::uint64_t until = next < script.size() ? script[next].frame : frames;
        while (frame < until) {
            const std::size_t n = std::min<std::uint64_t>(block_frames, until - frame);
            for (std::size_t c = 0; c < 4; ++c) {
                views.at(c) = std::span{out.at(c)}.subspan(frame, n);
            }
            mixer.process(Block{.channels = views,
                                .frames = n,
                                .sample_rate = kRate,
                                .host_time = {},
                                .resumed = false});
            frame += n;
        }
    }
    return out;
}

/// A graph that applies a script's controls at the start of the first block
/// at or after each one's frame, then runs the mixer.
class ScriptedMixer final : public Graph {
public:
    ScriptedMixer(Mixer& mixer, std::span<const MixerControl> script)
        : mixer_{&mixer}, script_{script} {}

    /// Replaces the script. Call it only before anything renders.
    void set_script(std::span<const MixerControl> script) noexcept { script_ = script; }

    void process(const Block& block) noexcept KS_NONBLOCKING override {
        while (next_ < script_.size() && script_[next_].frame <= frame_) {
            apply(*mixer_, script_[next_++]);
        }
        mixer_->process(block);
        frame_ += block.frames;
    }

private:
    Mixer* mixer_;
    std::span<const MixerControl> script_;
    std::size_t next_ = 0;
    std::uint64_t frame_ = 0;
};

} // namespace ks::engine::test
