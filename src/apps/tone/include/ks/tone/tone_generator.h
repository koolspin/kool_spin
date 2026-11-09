// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ks/core/realtime.h>
#include <ks/dsp/sine_oscillator.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ks::tone {

/// What a test tone sounds like.
struct ToneSettings {
    double frequency_hz = 440.0;
    double level_dbfs = -20.0;  ///< Peak level. -20 dBFS is comfortable on headphones.
    double fade_seconds = 0.01; ///< Fade at start and stop, so the tone never clicks.
};

/// Renders a sine tone into interleaved buffers on the audio thread.
///
/// Output starts with a fade-in. request_stop() starts a fade-out; once it
/// completes, finished() is true and the output is silence.
///
/// Threading: render() runs on the audio thread. The other member functions
/// can be called from any thread at any time.
class ToneGenerator {
public:
    /// @pre sample_rate > 2 * settings.frequency_hz, channels > 0, settings.fade_seconds > 0
    ToneGenerator(const ToneSettings& settings, double sample_rate, std::size_t channels) noexcept;

    /// Fills `interleaved` with whole frames of the tone, every channel identical.
    /// @pre interleaved.size() is a multiple of the channel count.
    void render(std::span<float> interleaved) noexcept KS_NONBLOCKING;

    /// Starts the fade-out. Calling it again has no further effect.
    void request_stop() noexcept;

    /// True once the fade-out has completed.
    [[nodiscard]] bool finished() const noexcept;

    /// Frames rendered so far.
    [[nodiscard]] std::uint64_t frames_rendered() const noexcept;

    /// Largest absolute sample value rendered so far.
    [[nodiscard]] float peak() const noexcept;

private:
    // Used only by the audio thread.
    dsp::SineOscillator oscillator_;
    std::size_t channels_;
    float amplitude_;
    float gain_step_; // gain change per frame while fading
    float gain_ = 0.0F;
    bool fading_out_ = false;

    // Shared with other threads.
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> finished_{false};
    std::atomic<std::uint64_t> frames_rendered_{0};
    std::atomic<float> peak_{0.0F};
};

} // namespace ks::tone
