// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/mixer.h>

#include <algorithm>
#include <cmath>
#include <span>

namespace ks::engine {

// The controls are implemented and validated; mixing is still a stub that
// writes nothing (specs/mixer/plan.md, task 2).

static_assert(kDecks == 4, "the constructor builds the decks one by one");

Mixer::Mixer(const Interpolator& interpolator, const BusMap& map) noexcept
    : decks_{Deck{interpolator}, Deck{interpolator}, Deck{interpolator}, Deck{interpolator}} {
    if (valid(map)) {
        settings_.bus_map = map;
    }
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

void Mixer::process(const Block& /*block*/) noexcept KS_NONBLOCKING {}

} // namespace ks::engine
