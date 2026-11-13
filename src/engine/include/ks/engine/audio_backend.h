// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The interface for drivers that render on their own thread, such as a
/// device (ADR 0003 §1).

#include <ks/engine/render_core.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace ks::engine {

/// What a backend is playing through.
struct StreamInfo {
    std::string backend;
    std::string device;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t period_frames = 0;
    /// The device's buffer, in periods; 0 if the backend doesn't say.
    std::uint32_t periods = 0;
    /// The backend's estimate of how long after a block's host time its first
    /// frame leaves the device; 0 if it has none
    /// (specs/miniaudio-devices/spec.md requirement 7).
    std::chrono::nanoseconds output_latency{0};
};

struct BackendError {
    enum class Kind : std::uint8_t {
        /// No audio backend could be initialized.
        NoBackend,
        /// The device couldn't be opened.
        OpenFailed,
        /// The device has, or was asked for, more than kMaxOutputChannels.
        TooManyChannels,
        /// The device wouldn't start.
        StartFailed,
    };

    Kind kind;
    std::string message;
};

/// A driver that renders on its own thread. The control thread starts and
/// stops it.
class AudioBackend {
public:
    AudioBackend() = default;
    AudioBackend(const AudioBackend&) = delete;
    AudioBackend& operator=(const AudioBackend&) = delete;
    AudioBackend(AudioBackend&&) = delete;
    AudioBackend& operator=(AudioBackend&&) = delete;
    /// Stops the backend.
    virtual ~AudioBackend() = default;

    /// Starts rendering, or resumes it after stop().
    virtual std::expected<void, BackendError> start() = 0;

    /// Stops rendering. Once it returns, the backend doesn't render again
    /// until start() is called.
    virtual void stop() noexcept = 0;

    [[nodiscard]] virtual const StreamInfo& info() const noexcept = 0;

    /// True once the backend knows its device has gone. Any thread may call
    /// it.
    [[nodiscard]] virtual bool lost() const noexcept = 0;

    /// How many times it has asked the core to render (device callbacks, or
    /// paced periods), whether or not anything rendered. Any thread may call
    /// it.
    [[nodiscard]] virtual std::uint64_t callbacks() const noexcept = 0;
};

/// How the output controller reaches the device: whether it's there, and
/// opening it.
class DeviceSource {
public:
    DeviceSource() = default;
    DeviceSource(const DeviceSource&) = delete;
    DeviceSource& operator=(const DeviceSource&) = delete;
    DeviceSource(DeviceSource&&) = delete;
    DeviceSource& operator=(DeviceSource&&) = delete;
    virtual ~DeviceSource() = default;

    /// Whether the device is there to open.
    [[nodiscard]] virtual bool available() = 0;

    /// Opens the device, not started, to render through `core` as `driver`.
    virtual std::expected<std::unique_ptr<AudioBackend>, BackendError> open(RenderCore& core,
                                                                            DriverId driver) = 0;
};

} // namespace ks::engine
