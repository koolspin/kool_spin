// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/deck.h>

#include <ks/engine/block.h>

#include <algorithm>

namespace ks::engine {

// Stubs: the acceptance tests compile against this API before it's
// implemented (specs/deck/spec.md). The deck renders silence and its controls
// only record their values.

Deck::Deck(const Interpolator& interpolator) noexcept : interpolator_{&interpolator} {}

bool Deck::load(const Track& track) noexcept KS_NONBLOCKING {
    track_ = &track;
    return true;
}

void Deck::unload() noexcept KS_NONBLOCKING {
    track_ = nullptr;
}

void Deck::set_playing(bool playing) noexcept KS_NONBLOCKING {
    playing_ = playing;
}

void Deck::set_rate(double rate) noexcept KS_NONBLOCKING {
    rate_ = rate;
}

void Deck::seek(double /*frame*/) noexcept KS_NONBLOCKING {}

void Deck::set_cue(double /*frame*/) noexcept KS_NONBLOCKING {}

void Deck::render(std::span<float> left, std::span<float> right,
                  std::uint32_t /*sample_rate*/) noexcept KS_NONBLOCKING {
    static_cast<void>(interpolator_);
    if (left.size() != right.size() || left.empty() || left.size() > kMaxBlockFrames) {
        return;
    }
    std::ranges::fill(left, 0.0F);
    std::ranges::fill(right, 0.0F);
}

const Track* Deck::track() const noexcept KS_NONBLOCKING {
    return track_;
}

bool Deck::holds(const Track& track) const noexcept KS_NONBLOCKING {
    return track_ == &track;
}

bool Deck::playing() const noexcept KS_NONBLOCKING {
    return playing_;
}

bool Deck::waiting() const noexcept KS_NONBLOCKING {
    return waiting_;
}

double Deck::rate() const noexcept KS_NONBLOCKING {
    return rate_;
}

double Deck::position() const noexcept KS_NONBLOCKING {
    return static_cast<double>(position_) / static_cast<double>(kOneFrame);
}

double Deck::cue() const noexcept KS_NONBLOCKING {
    return static_cast<double>(cue_) / static_cast<double>(kOneFrame);
}

} // namespace ks::engine
