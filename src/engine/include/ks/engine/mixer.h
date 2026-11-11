// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The mixer: four decks into a main bus and a headphone bus, with a trim,
/// channel fader, crossfader assignment, and cue button per channel
/// (specs/mixer/spec.md).

#include <ks/core/realtime.h>
#include <ks/engine/block.h>
#include <ks/engine/deck.h>
#include <ks/engine/interpolator.h>
#include <ks/engine/render_core.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace ks::engine {

/// The decks a mixer owns. Channel i of the mixer is deck i.
inline constexpr std::size_t kDecks = 4;

/// The time constant of gain smoothing.
inline constexpr std::chrono::milliseconds kGainSmoothing{1};

/// The most a trim boosts, in dB.
inline constexpr double kMaxTrimDb = 12.0;

/// The narrowest crossfader transition: the sharpest cut.
inline constexpr double kMinCrossfaderTransition = 0.01;

/// Which side of the crossfader a channel is on.
enum class CrossfaderSide : std::uint8_t {
    A,
    B,
    /// Not affected by the crossfader.
    Thru,
};

/// Where each bus goes: the first of two consecutive device channels. A map
/// is valid when both are even, below kMaxOutputChannels, and different.
struct BusMap {
    std::size_t main = 0;
    /// The headphone bus.
    std::size_t cue = 2;

    bool operator==(const BusMap&) const = default;
};

/// One channel's controls.
struct ChannelSettings {
    double trim_db = 0.0;
    /// [0, 1]; the gain is fader².
    double fader = 1.0;
    CrossfaderSide side = CrossfaderSide::Thru;
    bool cue = false;

    bool operator==(const ChannelSettings&) const = default;
};

/// Every control's current value: what was set, not the smoothed gains.
struct MixerSettings {
    std::array<ChannelSettings, kDecks> channels{};
    /// [0, 1]: 0 is all side A.
    double crossfader = 0.5;
    /// [kMinCrossfaderTransition, 1]: 1 is a constant-power blend.
    double crossfader_transition = 1.0;
    double main_level_db = 0.0;
    double headphone_level_db = 0.0;
    /// [0, 1]: 0 is the cue bus only in the headphones, 1 the main bus only.
    double cue_mix = 0.0;
    BusMap bus_map{};

    bool operator==(const MixerSettings&) const = default;
};

/// Mixes kDecks decks into a main bus and a headphone bus, and writes each
/// bus to a pair of the block's channels.
///
/// Controls are called on the audio thread between blocks, and take effect
/// from the next frame. Controls that take a deck index ignore an index of
/// kDecks or more; controls that take a double ignore NaN and clamp other
/// values. Every gain is smoothed per sample.
class Mixer final : public Graph {
public:
    /// Builds kDecks decks that use `interpolator`, which must outlive the
    /// mixer. An invalid `map` is replaced by the default. Everything the
    /// mixer uses is allocated by the time it returns.
    explicit Mixer(const Interpolator& interpolator, const BusMap& map = {}) noexcept;

    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;
    Mixer(Mixer&&) = delete;
    Mixer& operator=(Mixer&&) = delete;
    ~Mixer() = default;

    /// Deck `index`. @pre index < kDecks
    [[nodiscard]] Deck& deck(std::size_t index) noexcept KS_NONBLOCKING;
    [[nodiscard]] const Deck& deck(std::size_t index) const noexcept KS_NONBLOCKING;

    /// dB, at most kMaxTrimDb; −∞ silences the channel.
    void set_trim(std::size_t deck, double db) noexcept KS_NONBLOCKING;
    /// [0, 1].
    void set_fader(std::size_t deck, double position) noexcept KS_NONBLOCKING;
    void set_crossfader_assign(std::size_t deck, CrossfaderSide side) noexcept KS_NONBLOCKING;
    /// Sends the channel, pre-fader, to the headphone bus.
    void set_cue(std::size_t deck, bool on) noexcept KS_NONBLOCKING;

    /// [0, 1]: 0 is all side A.
    void set_crossfader(double position) noexcept KS_NONBLOCKING;
    /// [kMinCrossfaderTransition, 1].
    void set_crossfader_curve(double transition) noexcept KS_NONBLOCKING;

    /// dB, at most 0; −∞ silences the main output.
    void set_main_level(double db) noexcept KS_NONBLOCKING;
    /// dB, at most 0; −∞ silences the headphones.
    void set_headphone_level(double db) noexcept KS_NONBLOCKING;
    /// [0, 1]: 0 is the cue bus only, 1 the main bus only.
    void set_cue_mix(double position) noexcept KS_NONBLOCKING;

    /// Takes effect from the next block, without smoothing. An invalid map
    /// is ignored.
    void set_bus_map(const BusMap& map) noexcept KS_NONBLOCKING;

    [[nodiscard]] const MixerSettings& settings() const noexcept KS_NONBLOCKING;

    /// Renders every deck, mixes the buses, and writes each bus whose two
    /// channels exist in the block. Writes no other channel.
    void process(const Block& block) noexcept KS_NONBLOCKING override;

private:
    /// Whether `map` is valid: both channels even, below kMaxOutputChannels,
    /// and different.
    [[nodiscard]] static bool valid(const BusMap& map) noexcept KS_NONBLOCKING;

    /// The mixer's gains (spec requirement 9).
    struct Gains {
        /// aᵢ: each channel's gain into the main bus.
        std::array<double, kDecks> channel{};
        /// cᵢ: each channel's gain into the cue bus.
        std::array<double, kDecks> cue{};
        /// m: the main level.
        double main = 0.0;
        /// h_c: the cue bus's gain into the headphones.
        double headphone_cue = 0.0;
        /// h_m: the main bus's gain into the headphones.
        double headphone_main = 0.0;
    };

    /// One deck's output for the current block.
    struct DeckBuffer {
        std::array<float, kMaxBlockFrames> left{};
        std::array<float, kMaxBlockFrames> right{};
    };

    /// The gains the controls in settings_ ask for.
    [[nodiscard]] Gains targets() const noexcept KS_NONBLOCKING;

    /// Moves each gain one frame towards `target` (spec requirement 10).
    void smooth_towards(const Gains& target) noexcept KS_NONBLOCKING;

    std::array<Deck, kDecks> decks_;
    MixerSettings settings_;
    /// The gains in use, smoothed per sample towards targets() and carried
    /// across blocks.
    Gains gains_;
    /// The fraction of the remaining distance a gain moves each frame.
    double smoothing_ = 0.0;
    /// The sample rate smoothing_ was computed for; 0 before the first block.
    std::uint32_t sample_rate_ = 0;
    std::array<DeckBuffer, kDecks> buffers_{};
};

} // namespace ks::engine
