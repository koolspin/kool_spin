// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ks/tone/tone_generator.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace ks::tone {

/// The output device a tone plays on.
struct DeviceInfo {
    std::string backend; ///< As miniaudio names it, for example "PulseAudio" or "Null".
    std::string device;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t period_frames = 0; ///< Frames per audio callback.
    std::uint32_t periods = 0;
};

/// What happened during playback.
struct PlaybackReport {
    DeviceInfo device;
    std::uint64_t frames_rendered = 0;
    float peak = 0.0F;        ///< Largest absolute sample value played.
    double measured_hz = 0.0; ///< Frequency measured from the samples played.
};

/// Backend names accepted by play_tone().
[[nodiscard]] std::span<const std::string_view> backend_names() noexcept;

/// Plays a tone on the default output device for `duration`, then fades it out.
///
/// `backend` is one of backend_names(); "null" needs no audio hardware and is
/// meant for tests. An empty `backend` uses the first backend that works.
/// `on_start`, if set, is called with the device details once playback starts.
/// Returns an error message if the backend or device can't be opened.
[[nodiscard]] std::expected<PlaybackReport, std::string>
play_tone(std::string_view backend, const ToneSettings& settings,
          std::chrono::duration<double> duration,
          const std::function<void(const DeviceInfo&)>& on_start = {});

} // namespace ks::tone
