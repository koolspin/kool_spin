// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// A deck: plays a track from memory, with play and pause, seeks, a cue
/// point, and variable rate including reverse (specs/deck/spec.md).

#include <ks/core/realtime.h>
#include <ks/engine/interpolator.h>
#include <ks/engine/track.h>

#include <array>
#include <chrono>
#include <cstddef>
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
    /// A read position in a track (spec requirement 12).
    struct Head {
        const Track* track = nullptr;
        TrackPosition position = 0;
        /// p: the gain is p / ramp_frames_.
        std::uint64_t gain = 0;
    };

    /// A track's decoding progress, read once per block (requirement 18).
    struct Ready {
        const Track* track = nullptr;
        TrackReady ready;
    };

    /// Works out the per-rate values for `sample_rate`.
    void set_sample_rate(std::uint32_t sample_rate) noexcept KS_NONBLOCKING;

    /// Moves the main head to `position` in `track`, as a seek or load does:
    /// at once if it's silent, or as a crossfade. Returns false, changing
    /// nothing, if a crossfade is already running.
    bool move_main(const Track* track, TrackPosition position) noexcept KS_NONBLOCKING;

    /// Loads `track`, or makes the load wait for the current crossfade.
    void request_load(const Track* track) noexcept KS_NONBLOCKING;

    /// Applies a waiting load, then a waiting seek. Called once the outgoing
    /// head is dropped.
    void apply_waiting_controls() noexcept KS_NONBLOCKING;

    /// The track that controls apply to: a waiting load's, or the main head's.
    [[nodiscard]] const Track* control_track() const noexcept KS_NONBLOCKING;

    /// One frame: the rate, the output, the heads' motion, and the gains.
    [[nodiscard]] StereoSample render_frame() noexcept KS_NONBLOCKING;

    /// The value at `head`'s position, at a step of `step` frames.
    [[nodiscard]] StereoSample value_at(const Head& head, double step) noexcept KS_NONBLOCKING;

    /// Moves the main head by `step`, applying the ends and the waiting
    /// limit (requirement 17).
    void advance_main(TrackPosition step) noexcept KS_NONBLOCKING;

    /// The gain of the main head, or of the outgoing one.
    [[nodiscard]] float gain_of(const Head& head, bool main) const noexcept KS_NONBLOCKING;

    /// `track`'s decoding progress, read at most once per block.
    [[nodiscard]] TrackReady ready_of(const Track& track) noexcept KS_NONBLOCKING;

    const Interpolator* interpolator_;
    Head main_;
    Head outgoing_;
    bool has_outgoing_ = false;

    /// Controls waiting for the current crossfade to finish.
    bool load_pending_ = false;
    const Track* pending_track_ = nullptr;
    bool seek_pending_ = false;
    TrackPosition pending_seek_ = 0;

    bool playing_ = false;
    bool waiting_ = false;
    double target_rate_ = 1.0;
    double rate_ = 1.0;
    TrackPosition cue_ = 0;

    /// The sample rate the values below are for.
    std::uint32_t sample_rate_ = 0;
    /// n: kDeclickTime in frames.
    std::uint64_t ramp_frames_ = 0;
    /// 1 − e^(−1 / (kRateSmoothing × sample rate)).
    double smoothing_ = 1.0;

    /// The tracks whose progress this block has read: at most the main
    /// head's, the outgoing head's, and a waiting load's.
    std::array<Ready, 3> ready_{};
    std::size_t ready_count_ = 0;
};

} // namespace ks::engine
