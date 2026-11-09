// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ks/tone/tone_generator.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

/// One output device, as --list-devices shows it.
struct OutputDevice {
    std::string name;
    bool is_default = false; ///< The system default output device.
};

/// A backend's output devices, in miniaudio's order.
struct DeviceList {
    std::string backend; ///< As miniaudio names it, for example "PulseAudio" or "Null".
    std::vector<OutputDevice> devices;
};

/// Why play_tone() failed.
struct PlayError {
    enum class Kind : std::uint8_t {
        Usage,   ///< The request can't be met, for example a device number that doesn't exist.
        Runtime, ///< The backend or device failed.
    };
    Kind kind = Kind::Runtime;
    std::string message; ///< One line, without the "ks-tone: " prefix.
};

/// Backend names accepted by play_tone().
[[nodiscard]] std::span<const std::string_view> backend_names() noexcept;

/// Lists the output devices of `backend`, chosen the same way play_tone()
/// chooses it: an empty `backend` uses the first backend that works.
/// Returns an error message if the backend can't be opened or queried.
[[nodiscard]] std::expected<DeviceList, std::string> list_devices(std::string_view backend);

/// Plays a tone for `duration`, then fades it out.
///
/// `backend` is one of backend_names(); "null" needs no audio hardware and is
/// meant for tests. An empty `backend` uses the first backend that works.
/// `device_index` is an index into list_devices()' devices; unset plays on
/// the system default device. An index past the end is a usage error,
/// reported before any device is opened.
/// `on_start`, if set, is called with the device details once playback starts.
/// Returns an error if the backend or device can't be opened.
[[nodiscard]] std::expected<PlaybackReport, PlayError>
play_tone(std::string_view backend, const ToneSettings& settings,
          std::chrono::duration<double> duration, std::optional<std::uint32_t> device_index = {},
          const std::function<void(const DeviceInfo&)>& on_start = {});

} // namespace ks::tone
