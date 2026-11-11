// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/mixer.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>

namespace ks::engine {

// The controls and mixing are implemented; gains still jump to their targets
// at the start of each block instead of being smoothed per sample
// (specs/mixer/plan.md, task 3).

namespace {

/// 10^(db / 20); −∞ gives exactly 0.
double gain_of_db(double db) noexcept KS_NONBLOCKING {
    return std::pow(10.0, db / 20.0);
}

/// Side A's crossfader gain at `position` with `transition` (spec
/// requirement 4).
double side_a(double position, double transition) noexcept KS_NONBLOCKING {
    if (position <= 1.0 - transition) {
        return 1.0;
    }
    return std::cos(std::numbers::pi / 2.0 * (position - (1.0 - transition)) / transition);
}

/// A channel's crossfader gain.
double crossfader_gain(CrossfaderSide side, double position,
                       double transition) noexcept KS_NONBLOCKING {
    switch (side) {
    case CrossfaderSide::A:
        return side_a(position, transition);
    case CrossfaderSide::B:
        return side_a(1.0 - position, transition);
    case CrossfaderSide::Thru:
        return 1.0;
    }
    return 1.0;
}

/// The first `frames` samples of channels `first` and `first + 1`, or empty
/// spans if the block doesn't have both.
struct Pair {
    std::span<float> left;
    std::span<float> right;
    bool present = false;
    /// The block has both channels, but one is shorter than the block.
    bool short_channel = false;
};

Pair pair_of(std::span<const std::span<float>> channels, std::size_t first,
             std::size_t frames) noexcept KS_NONBLOCKING {
    if (first + 1 >= channels.size()) {
        return {};
    }
    const std::span<float> left = channels[first];
    const std::span<float> right = channels[first + 1];
    if (left.size() < frames || right.size() < frames) {
        return {.left = {}, .right = {}, .present = false, .short_channel = true};
    }
    return {.left = left.first(frames),
            .right = right.first(frames),
            .present = true,
            .short_channel = false};
}

} // namespace

static_assert(kDecks == 4, "the constructor builds the decks one by one");

Mixer::Mixer(const Interpolator& interpolator, const BusMap& map) noexcept
    : decks_{Deck{interpolator}, Deck{interpolator}, Deck{interpolator}, Deck{interpolator}} {
    if (valid(map)) {
        settings_.bus_map = map;
    }
    gains_ = targets();
}

Mixer::Gains Mixer::targets() const noexcept KS_NONBLOCKING {
    Gains gains;
    for (std::size_t i = 0; i < kDecks; ++i) {
        const ChannelSettings& channel = std::span{settings_.channels}[i];
        const double trim = gain_of_db(channel.trim_db);
        std::span{gains.channel}[i] =
            trim * (channel.fader * channel.fader) *
            crossfader_gain(channel.side, settings_.crossfader, settings_.crossfader_transition);
        std::span{gains.cue}[i] = channel.cue ? trim : 0.0;
    }
    gains.main = gain_of_db(settings_.main_level_db);
    const double headphone = gain_of_db(settings_.headphone_level_db);
    gains.headphone_cue = headphone * (1.0 - settings_.cue_mix);
    gains.headphone_main = headphone * settings_.cue_mix;
    return gains;
}

bool Mixer::valid(const BusMap& map) noexcept KS_NONBLOCKING {
    return map.main % 2 == 0 && map.cue % 2 == 0 && map.main < kMaxOutputChannels &&
           map.cue < kMaxOutputChannels && map.main != map.cue;
}

Deck& Mixer::deck(std::size_t index) noexcept KS_NONBLOCKING {
    return std::span{decks_}[index];
}

const Deck& Mixer::deck(std::size_t index) const noexcept KS_NONBLOCKING {
    return std::span{decks_}[index];
}

void Mixer::set_trim(std::size_t deck, double db) noexcept KS_NONBLOCKING {
    if (deck >= kDecks || std::isnan(db)) {
        return;
    }
    std::span{settings_.channels}[deck].trim_db = std::min(db, kMaxTrimDb);
}

void Mixer::set_fader(std::size_t deck, double position) noexcept KS_NONBLOCKING {
    if (deck >= kDecks || std::isnan(position)) {
        return;
    }
    std::span{settings_.channels}[deck].fader = std::clamp(position, 0.0, 1.0);
}

void Mixer::set_crossfader_assign(std::size_t deck, CrossfaderSide side) noexcept KS_NONBLOCKING {
    if (deck >= kDecks) {
        return;
    }
    std::span{settings_.channels}[deck].side = side;
}

void Mixer::set_cue(std::size_t deck, bool on) noexcept KS_NONBLOCKING {
    if (deck >= kDecks) {
        return;
    }
    std::span{settings_.channels}[deck].cue = on;
}

void Mixer::set_crossfader(double position) noexcept KS_NONBLOCKING {
    if (std::isnan(position)) {
        return;
    }
    settings_.crossfader = std::clamp(position, 0.0, 1.0);
}

void Mixer::set_crossfader_curve(double transition) noexcept KS_NONBLOCKING {
    if (std::isnan(transition)) {
        return;
    }
    settings_.crossfader_transition = std::clamp(transition, kMinCrossfaderTransition, 1.0);
}

void Mixer::set_main_level(double db) noexcept KS_NONBLOCKING {
    if (std::isnan(db)) {
        return;
    }
    settings_.main_level_db = std::min(db, 0.0);
}

void Mixer::set_headphone_level(double db) noexcept KS_NONBLOCKING {
    if (std::isnan(db)) {
        return;
    }
    settings_.headphone_level_db = std::min(db, 0.0);
}

void Mixer::set_cue_mix(double position) noexcept KS_NONBLOCKING {
    if (std::isnan(position)) {
        return;
    }
    settings_.cue_mix = std::clamp(position, 0.0, 1.0);
}

void Mixer::set_bus_map(const BusMap& map) noexcept KS_NONBLOCKING {
    if (valid(map)) {
        settings_.bus_map = map;
    }
}

const MixerSettings& Mixer::settings() const noexcept KS_NONBLOCKING {
    return settings_;
}

void Mixer::process(const Block& block) noexcept KS_NONBLOCKING {
    const std::size_t frames = block.frames;
    if (frames == 0 || frames > kMaxBlockFrames || block.sample_rate == 0) {
        return;
    }
    // A map change applies from the next block.
    const BusMap map = settings_.bus_map;
    const Pair main_out = pair_of(block.channels, map.main, frames);
    const Pair cue_out = pair_of(block.channels, map.cue, frames);
    if (main_out.short_channel || cue_out.short_channel) {
        return;
    }

    for (std::size_t i = 0; i < kDecks; ++i) {
        DeckBuffer& buffer = std::span{buffers_}[i];
        std::span{decks_}[i].render(std::span{buffer.left}.first(frames),
                                    std::span{buffer.right}.first(frames), block.sample_rate);
    }

    gains_ = targets();
    const std::span<const double, kDecks> channel_gains{gains_.channel};
    const std::span<const double, kDecks> cue_gains{gains_.cue};

    for (std::size_t frame = 0; frame < frames; ++frame) {
        double main_left = 0.0;
        double main_right = 0.0;
        double cue_left = 0.0;
        double cue_right = 0.0;
        for (std::size_t i = 0; i < kDecks; ++i) {
            const DeckBuffer& buffer = std::span{buffers_}[i];
            const auto left = static_cast<double>(std::span{buffer.left}[frame]);
            const auto right = static_cast<double>(std::span{buffer.right}[frame]);
            main_left += channel_gains[i] * left;
            main_right += channel_gains[i] * right;
            cue_left += cue_gains[i] * left;
            cue_right += cue_gains[i] * right;
        }
        if (main_out.present) {
            main_out.left[frame] = static_cast<float>(gains_.main * main_left);
            main_out.right[frame] = static_cast<float>(gains_.main * main_right);
        }
        if (cue_out.present) {
            cue_out.left[frame] = static_cast<float>((gains_.headphone_cue * cue_left) +
                                                     (gains_.headphone_main * main_left));
            cue_out.right[frame] = static_cast<float>((gains_.headphone_cue * cue_right) +
                                                      (gains_.headphone_main * main_right));
        }
    }
}

} // namespace ks::engine
