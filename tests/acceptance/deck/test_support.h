// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the deck acceptance tests: tracks in private memory, rendering a
// deck in blocks, measuring tones, and the performance script.

#include <ks/core/realtime.h>
#include <ks/engine/block.h>
#include <ks/engine/deck.h>
#include <ks/engine/render_core.h>
#include <ks/engine/track.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

namespace ks::engine::test {

/// The device rate most tests render at.
inline constexpr std::uint32_t kRate = 48'000;
/// A 5 ms ramp at kRate.
inline constexpr std::size_t kRampFrames = 240;
/// Absolute tolerance for float comparisons.
inline constexpr double kTolerance = 1e-6;

inline constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

/// A value for each frame and channel, different from its neighbors, so a
/// sample in the wrong place can't pass a check by accident. In [-0.5, 0.5).
constexpr float signal(std::uint64_t frame, std::size_t channel) noexcept {
    std::uint64_t x = (frame * 0x9E37'79B9'7F4A'7C15ULL) + (channel * 0xBF58'476D'1CE4'E5B9ULL) + 1;
    x ^= x >> 31U;
    x *= 0x94D0'49BB'1331'11EBULL;
    x ^= x >> 29U;
    return (static_cast<float>(x >> 40U) / 16'777'216.0F) - 0.5F;
}

/// signal(), as a function object.
inline constexpr auto kSignal = [](std::uint64_t frame, std::size_t channel) {
    return signal(frame, channel);
};

/// A track in private memory, filled with value(frame, channel). Memory on
/// either side of it, `guard_frames` frames each, holds NaN, so reading
/// outside the track shows up. Every frame is published as ready and
/// complete.
class OwnedTrack {
public:
    template <typename Value>
    OwnedTrack(std::uint64_t capacity, std::uint32_t sample_rate, Value value,
               std::size_t guard_frames = 0)
        : samples_((capacity + (2 * guard_frames)) * 2, kNaN),
          track_{std::span<const float>{samples_}.subspan(guard_frames * 2, capacity * 2),
                 sample_rate},
          guard_frames_{guard_frames} {
        for (std::uint64_t frame = 0; frame < capacity; ++frame) {
            for (std::size_t channel = 0; channel < 2; ++channel) {
                samples_.at(index(frame, channel)) = value(frame, channel);
            }
        }
        track_.publish(capacity, true);
    }

    OwnedTrack(const OwnedTrack&) = delete;
    OwnedTrack& operator=(const OwnedTrack&) = delete;
    OwnedTrack(OwnedTrack&&) = delete;
    OwnedTrack& operator=(OwnedTrack&&) = delete;
    ~OwnedTrack() = default;

    [[nodiscard]] Track& track() noexcept { return track_; }

    /// The sample at `frame` of `channel`.
    [[nodiscard]] float sample(std::uint64_t frame, std::size_t channel) const {
        return samples_.at(index(frame, channel));
    }

private:
    [[nodiscard]] std::size_t index(std::uint64_t frame, std::size_t channel) const {
        return ((guard_frames_ + frame) * 2) + channel;
    }

    std::vector<float> samples_;
    Track track_;
    std::size_t guard_frames_;
};

/// A deck's output, and optionally its position before each frame.
struct Stereo {
    std::vector<float> left;
    std::vector<float> right;
    /// position() before each frame, when rendered one frame at a time.
    std::vector<double> positions;

    [[nodiscard]] float at(std::size_t channel, std::size_t frame) const {
        return channel == 0 ? left.at(frame) : right.at(frame);
    }

    /// True if every sample is finite.
    [[nodiscard]] bool finite() const {
        const auto is_finite = [](float sample) { return std::isfinite(sample); };
        return std::ranges::all_of(left, is_finite) && std::ranges::all_of(right, is_finite);
    }

    /// True if every sample is exactly 0.
    [[nodiscard]] bool silent() const {
        const auto is_zero = [](float sample) { return sample == 0.0F; };
        return std::ranges::all_of(left, is_zero) && std::ranges::all_of(right, is_zero);
    }
};

/// Renders `frames` frames of `deck` in blocks of `block_frames`.
inline Stereo render(Deck& deck, std::size_t frames, std::size_t block_frames = 64,
                     std::uint32_t sample_rate = kRate) {
    Stereo out{.left = std::vector<float>(frames, 5.0F),
               .right = std::vector<float>(frames, 5.0F),
               .positions = {}};
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min(block_frames, frames - done);
        deck.render(std::span{out.left}.subspan(done, n), std::span{out.right}.subspan(done, n),
                    sample_rate);
        done += n;
    }
    return out;
}

/// Renders `frames` frames one at a time, recording position() before each.
inline Stereo render_tracking(Deck& deck, std::size_t frames, std::uint32_t sample_rate = kRate) {
    Stereo out{.left = std::vector<float>(frames, 5.0F),
               .right = std::vector<float>(frames, 5.0F),
               .positions = std::vector<double>(frames)};
    for (std::size_t frame = 0; frame < frames; ++frame) {
        out.positions.at(frame) = deck.position();
        deck.render(std::span{out.left}.subspan(frame, 1), std::span{out.right}.subspan(frame, 1),
                    sample_rate);
    }
    return out;
}

/// True if `actual` is within kTolerance of `expected`.
inline bool near(float actual, double expected) {
    return std::abs(static_cast<double>(actual) - expected) <= kTolerance;
}

// Measuring tones.

/// The result of fitting a sine to a signal.
struct ToneFit {
    /// The fitted sine's amplitude.
    double amplitude = 0.0;
    /// The power left after subtracting the fit, relative to the sine's, in dB.
    double residual_db = 0.0;
};

/// Fits a·sin(ωn) + b·cos(ωn) + c to `samples` by least squares, with ω in
/// radians per frame.
inline ToneFit fit_tone(std::span<const float> samples, double omega) {
    // The normal equations, solved by Gaussian elimination.
    std::array<std::array<double, 4>, 3> m{};
    for (std::size_t n = 0; n < samples.size(); ++n) {
        const double phase = omega * static_cast<double>(n);
        const std::array<double, 3> basis{std::sin(phase), std::cos(phase), 1.0};
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t col = 0; col < 3; ++col) {
                m.at(row).at(col) += basis.at(row) * basis.at(col);
            }
            m.at(row).at(3) += basis.at(row) * static_cast<double>(samples[n]);
        }
    }
    for (std::size_t col = 0; col < 3; ++col) {
        for (std::size_t row = 0; row < 3; ++row) {
            if (row != col) {
                const double factor = m.at(row).at(col) / m.at(col).at(col);
                for (std::size_t k = 0; k < 4; ++k) {
                    m.at(row).at(k) -= factor * m.at(col).at(k);
                }
            }
        }
    }
    const double a = m.at(0).at(3) / m.at(0).at(0);
    const double b = m.at(1).at(3) / m.at(1).at(1);
    const double c = m.at(2).at(3) / m.at(2).at(2);

    double residual = 0.0;
    for (std::size_t n = 0; n < samples.size(); ++n) {
        const double phase = omega * static_cast<double>(n);
        const double error =
            static_cast<double>(samples[n]) - ((a * std::sin(phase)) + (b * std::cos(phase)) + c);
        residual += error * error;
    }
    const double amplitude = std::hypot(a, b);
    const double sine_power = amplitude * amplitude / 2.0 * static_cast<double>(samples.size());
    return ToneFit{.amplitude = amplitude,
                   .residual_db = 10.0 * std::log10((residual / sine_power) + 1e-30)};
}

/// The level of `samples` relative to a sine of amplitude `amplitude`, in dB.
inline double level_db(std::span<const float> samples, double amplitude) {
    double power = 0.0;
    for (const float sample : samples) {
        power += static_cast<double>(sample) * static_cast<double>(sample);
    }
    const double rms = std::sqrt(power / static_cast<double>(samples.size()));
    return 20.0 * std::log10((rms / (amplitude / std::numbers::sqrt2)) + 1e-30);
}

/// A sine of `frequency` Hz at amplitude 0.5, the same on both channels.
inline auto sine(double frequency, std::uint32_t sample_rate) {
    return [frequency, sample_rate](std::uint64_t frame, std::size_t /*channel*/) {
        return static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(frame) /
                           static_cast<double>(sample_rate)));
    };
}

// The performance script (specs/deck/spec.md).

/// The rate of the performance's track.
inline constexpr std::uint32_t kTrackRate = 44'100;
/// The performance's track: 3.5 s.
inline constexpr std::uint64_t kPerformanceTrackFrames = 154'350;
/// The performance: 3 s at kRate.
inline constexpr std::uint64_t kPerformanceFrames = 144'000;

/// Steady tones, with a decaying click every 0.25 s, different on each
/// channel. Peaks below 0.6.
inline float synthetic(std::uint64_t frame, std::size_t channel, std::uint32_t sample_rate) {
    const double t = static_cast<double>(frame) / static_cast<double>(sample_rate);
    const auto ch = static_cast<double>(channel);
    const double tau = 2.0 * std::numbers::pi;
    const double tones =
        (0.2 * std::sin((tau * 220.0 * t) + ch)) + (0.1 * std::sin((tau * 3'520.0 * t) + (2 * ch)));
    const double since_click = std::fmod(t, 0.25);
    const double click =
        0.3 * std::exp(-since_click / 0.01) * std::sin(tau * 1'000.0 * since_click);
    return static_cast<float>(tones + click);
}

enum class Action : std::uint8_t { Load, Play, Pause, Rate, Seek };

/// A control applied before the frame `frame` of the performance.
struct Control {
    std::uint64_t frame = 0;
    Action action = Action::Play;
    /// The rate, or the frame to seek to.
    double value = 0.0;
};

inline constexpr std::array<Control, 13> kPerformance{{
    {.frame = 0, .action = Action::Load, .value = 0.0},
    {.frame = 0, .action = Action::Seek, .value = 22'050.0}, // 0.5 s of track time
    {.frame = 0, .action = Action::Play, .value = 0.0},
    {.frame = 24'000, .action = Action::Rate, .value = 1.08},     // 0.5 s
    {.frame = 43'200, .action = Action::Seek, .value = 88'200.0}, // 0.9 s: to 2.0 s
    {.frame = 43'296, .action = Action::Seek, .value = 44'100.0}, // 0.902 s: to 1.0 s
    {.frame = 62'400, .action = Action::Rate, .value = -1.0},     // 1.3 s
    {.frame = 86'400, .action = Action::Pause, .value = 0.0},     // 1.8 s
    {.frame = 96'000, .action = Action::Rate, .value = 1.0},      // 2.0 s
    {.frame = 96'000, .action = Action::Play, .value = 0.0},
    {.frame = 105'600, .action = Action::Rate, .value = 4.0},       // 2.2 s
    {.frame = 115'200, .action = Action::Rate, .value = 1.0},       // 2.4 s
    {.frame = 115'200, .action = Action::Seek, .value = 149'940.0}, // to 3.4 s, near the end
}};

/// Applies one control.
inline void apply(Deck& deck, const Track& track, const Control& control) noexcept KS_NONBLOCKING {
    switch (control.action) {
    case Action::Load:
        static_cast<void>(deck.load(track));
        return;
    case Action::Play:
        deck.set_playing(true);
        return;
    case Action::Pause:
        deck.set_playing(false);
        return;
    case Action::Rate:
        deck.set_rate(control.value);
        return;
    case Action::Seek:
        deck.seek(control.value);
        return;
    }
}

/// What a deck reports.
struct DeckState {
    double position = 0.0;
    bool playing = false;
    bool waiting = false;
    double rate = 0.0;

    bool operator==(const DeckState&) const = default;
};

inline DeckState state_of(const Deck& deck) {
    return DeckState{.position = deck.position(),
                     .playing = deck.playing(),
                     .waiting = deck.waiting(),
                     .rate = deck.rate()};
}

/// The output of a script, and the deck's state before each control and at
/// the end.
struct Performance {
    Stereo output;
    std::vector<DeckState> states;
};

/// Renders `frames` frames of `script` directly through `deck`, in blocks of
/// at most `block_frames`, split so each control applies at its frame.
inline Performance perform(Deck& deck, const Track& track, std::span<const Control> script,
                           std::uint64_t frames, std::size_t block_frames) {
    Performance performance{.output = {.left = std::vector<float>(frames, 5.0F),
                                       .right = std::vector<float>(frames, 5.0F),
                                       .positions = {}},
                            .states = {}};
    std::size_t next = 0;
    for (std::uint64_t frame = 0; frame < frames;) {
        performance.states.push_back(state_of(deck));
        while (next < script.size() && script[next].frame <= frame) {
            apply(deck, track, script[next++]);
        }
        const std::uint64_t until = next < script.size() ? script[next].frame : frames;
        while (frame < until) {
            const std::size_t n = std::min<std::uint64_t>(block_frames, until - frame);
            deck.render(std::span{performance.output.left}.subspan(frame, n),
                        std::span{performance.output.right}.subspan(frame, n), kRate);
            frame += n;
        }
    }
    performance.states.push_back(state_of(deck));
    return performance;
}

/// A graph that plays a deck, applying a script's controls at the start of
/// the first block at or after each one's frame. Writes the first two
/// channels; a one-channel block gets the left channel.
class ScriptGraph final : public Graph {
public:
    ScriptGraph(Deck& deck, const Track& track, std::span<const Control> script)
        : deck_{&deck}, track_{&track}, script_{script} {}

    /// Replaces the script. Call it only before anything renders.
    void set_script(std::span<const Control> script) noexcept { script_ = script; }

    void process(const Block& block) noexcept KS_NONBLOCKING override {
        while (next_ < script_.size() && script_[next_].frame <= frame_) {
            apply(*deck_, *track_, script_[next_++]);
        }
        const std::span<float> left = block.channels[0].first(block.frames);
        const std::span<float> right = block.channels.size() > 1
                                           ? block.channels[1].first(block.frames)
                                           : std::span{scratch_}.first(block.frames);
        deck_->render(left, right, block.sample_rate);
        frame_ += block.frames;
    }

private:
    Deck* deck_;
    const Track* track_;
    std::span<const Control> script_;
    std::size_t next_ = 0;
    std::uint64_t frame_ = 0;
    std::array<float, kMaxBlockFrames> scratch_{};
};

} // namespace ks::engine::test
