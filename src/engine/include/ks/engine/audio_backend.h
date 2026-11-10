// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The interface for drivers that render on their own thread, such as a
/// device (ADR 0003 §1).

#include <cstdint>
#include <expected>
#include <string>

namespace ks::engine {

/// What a backend is playing through.
struct StreamInfo {
    std::string backend;
    std::string device;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t period_frames = 0;
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
};

} // namespace ks::engine
