// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/mixer.h>

#include <span>

namespace ks::engine {

// Stubs: the acceptance tests compile against this API before it's
// implemented (specs/mixer/spec.md). The decks exist, but the mixer ignores
// its controls and writes nothing.

static_assert(kDecks == 4, "the constructor builds the decks one by one");

Mixer::Mixer(const Interpolator& interpolator, const BusMap& /*map*/) noexcept
    : decks_{Deck{interpolator}, Deck{interpolator}, Deck{interpolator}, Deck{interpolator}} {}

Deck& Mixer::deck(std::size_t index) noexcept KS_NONBLOCKING {
    return std::span{decks_}[index];
}

const Deck& Mixer::deck(std::size_t index) const noexcept KS_NONBLOCKING {
    return std::span{decks_}[index];
}

void Mixer::set_trim(std::size_t /*deck*/, double /*db*/) noexcept KS_NONBLOCKING {}

void Mixer::set_fader(std::size_t /*deck*/, double /*position*/) noexcept KS_NONBLOCKING {}

void Mixer::set_crossfader_assign(std::size_t /*deck*/,
                                  CrossfaderSide /*side*/) noexcept KS_NONBLOCKING {}

void Mixer::set_cue(std::size_t /*deck*/, bool /*on*/) noexcept KS_NONBLOCKING {}

void Mixer::set_crossfader(double /*position*/) noexcept KS_NONBLOCKING {}

void Mixer::set_crossfader_curve(double /*transition*/) noexcept KS_NONBLOCKING {}

void Mixer::set_main_level(double /*db*/) noexcept KS_NONBLOCKING {}

void Mixer::set_headphone_level(double /*db*/) noexcept KS_NONBLOCKING {}

void Mixer::set_cue_mix(double /*position*/) noexcept KS_NONBLOCKING {}

void Mixer::set_bus_map(const BusMap& /*map*/) noexcept KS_NONBLOCKING {}

const MixerSettings& Mixer::settings() const noexcept KS_NONBLOCKING {
    return settings_;
}

void Mixer::process(const Block& /*block*/) noexcept KS_NONBLOCKING {}

} // namespace ks::engine
