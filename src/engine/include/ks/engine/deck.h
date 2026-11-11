// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// A deck: plays a track from memory, with play and pause, seeks, a cue
/// point, and variable rate including reverse (specs/deck/spec.md).

#include <ks/core/realtime.h>
#include <ks/engine/interpolator.h>
#include <ks/engine/track.h>

#include <chrono>
#include <cstdint>
#include <span>

namespace ks::engine {

/// How long gain ramps and seek crossfades take.
inline constexpr std::chrono::milliseconds kDeclickTime{5};
/// The time constant of rate smoothing.
inline constexpr std::chrono::milliseconds kRateSmoothing{5};
/// The fastest a deck plays, forward or in reverse.
inline constexpr double kMaxRate = 4.0;

/// Plays one track at a time.
///
/// The deck reads the track through a main head and, while a seek or load
/// crossfades, an outgoing head. It's used from one thread at a time, the
/// audio thread. Controls called between renders take effect from the next
/// rendered frame. It allocates nothing.
class Deck {
public:
    /// The interpolator must outlive the deck.
    explicit Deck(const Interpolator& interpolator) noexcept;

    Deck(const Deck&) = delete;
    Deck& operator=(const Deck&) = delete;
    Deck(Deck&&) = delete;
    Deck& operator=(Deck&&) = delete;
    ~Deck() = default;

    /// Loads `track`, paused at frame 0 with the cue point at 0: at once if
    /// the deck is silent, as a fade out if it's audible, or after the current
    /// crossfade. Returns false, and changes nothing, if the track's sample
    /// rate or capacity is out of range. The track must outlive the time the
    /// deck holds it (holds()).
    [[nodiscard]] bool load(const Track& track) noexcept KS_NONBLOCKING;

    /// Unloads the track, as load() would load an empty one.
    void unload() noexcept KS_NONBLOCKING;

    /// Plays or pauses, ramping the gain. Ignored with no track loaded.
    void set_playing(bool playing) noexcept KS_NONBLOCKING;

    /// Sets the target rate: 1 is normal speed, negative is reverse. NaN and
    /// infinity are ignored; other values are clamped to ±kMaxRate.
    void set_rate(double rate) noexcept KS_NONBLOCKING;

    /// Moves the main head to a track frame, crossfading if the deck is
    /// audible. NaN and infinity are ignored; other values are clamped to
    /// [0, capacity]. Ignored with no track loaded.
    void seek(double frame) noexcept KS_NONBLOCKING;

    /// Stores a cue point, validated like a seek. The deck doesn't act on it.
    void set_cue(double frame) noexcept KS_NONBLOCKING;

    /// Writes the deck's output, one frame per element of `left`, at
    /// `sample_rate`. Writes nothing and changes nothing if `left` and `right`
    /// differ in size, are empty, or hold more than kMaxBlockFrames, or if the
    /// sample rate is 0.
    void render(std::span<float> left, std::span<float> right,
                std::uint32_t sample_rate) noexcept KS_NONBLOCKING;

    /// The main head's track, or null.
    [[nodiscard]] const Track* track() const noexcept KS_NONBLOCKING;

    /// Whether either head, or a waiting load, refers to `track`.
    [[nodiscard]] bool holds(const Track& track) const noexcept KS_NONBLOCKING;

    [[nodiscard]] bool playing() const noexcept KS_NONBLOCKING;

    /// Whether the main head is held at the waiting limit of a track that's
    /// still decoding.
    [[nodiscard]] bool waiting() const noexcept KS_NONBLOCKING;

    /// The target rate.
    [[nodiscard]] double rate() const noexcept KS_NONBLOCKING;

    /// The main head's position, in track frames.
    [[nodiscard]] double position() const noexcept KS_NONBLOCKING;

    [[nodiscard]] double cue() const noexcept KS_NONBLOCKING;

private:
    const Interpolator* interpolator_;
    const Track* track_ = nullptr;
    bool playing_ = false;
    bool waiting_ = false;
    double rate_ = 1.0;
    TrackPosition position_ = 0;
    TrackPosition cue_ = 0;
};

} // namespace ks::engine
