// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Keeping the core rendering through device loss (ADR 0002 §4, ADR 0003 §15).

#include <ks/engine/audio_backend.h>
#include <ks/engine/render_core.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace ks::engine {

struct OutputConfig {
    /// A device whose callbacks stop for this long is lost.
    std::chrono::milliseconds stall_timeout{100};
    /// How often to look for a lost device's return.
    std::chrono::milliseconds retry_interval{500};
    /// The freewheel driver's period.
    std::size_t freewheel_period_frames = 256;
    /// The freewheel driver's format if no device has ever opened.
    std::uint32_t default_sample_rate = 48'000;
    std::uint32_t default_channels = 2;
};

enum class OutputState : std::uint8_t {
    Stopped,
    /// The device is the active driver.
    Playing,
    /// The device is lost; the freewheel driver keeps the clock running.
    Freewheeling,
    /// The device is back and started, but not yet active.
    Attaching,
};

/// Plays through the device the source opens, freewheels while it's lost, and
/// hands back when it returns. Used from one thread, the control thread,
/// which calls poll() at least every 10 ms.
class OutputController {
public:
    /// The core and the source must outlive the controller.
    OutputController(RenderCore& core, DeviceSource& source, const OutputConfig& config) noexcept;

    OutputController(const OutputController&) = delete;
    OutputController& operator=(const OutputController&) = delete;
    OutputController(OutputController&&) = delete;
    OutputController& operator=(OutputController&&) = delete;
    /// Stops whatever is running, as stop() does.
    ~OutputController();

    /// Unmutes the core, then opens and starts the device, or freewheels if
    /// it can't.
    void start();

    /// Notices a lost device, looks for its return, and hands back to it.
    void poll();

    /// Mutes the core, waits for the fade out (at most the fade time plus
    /// 100 ms), then stops and closes whatever is running.
    void stop();

    [[nodiscard]] OutputState state() const noexcept;

private:
    using Clock = std::chrono::steady_clock;

    /// A DriverId never given out before.
    DriverId next_driver() noexcept;

    /// Opens the device as `driver`, not started, or returns null if the
    /// source doesn't have it.
    std::unique_ptr<AudioBackend> open_device(DriverId driver);

    /// Makes `device` the controller's device, remembering its format, and
    /// restarts the stall timer.
    void adopt_device(std::unique_ptr<AudioBackend> device, DriverId driver,
                      Clock::time_point started);

    /// Opens, activates, and starts the freewheel driver, with its first
    /// period at `start_time`.
    void start_freewheel(Clock::time_point start_time);

    /// Stops and destroys `backend`, if there is one.
    static void close(std::unique_ptr<AudioBackend>& backend) noexcept;

    RenderCore* core_;
    DeviceSource* source_;
    OutputConfig config_;
    OutputState state_ = OutputState::Stopped;

    /// While Playing, the active device; while Attaching, the device waiting
    /// to become active.
    std::unique_ptr<AudioBackend> device_;
    std::unique_ptr<AudioBackend> freewheel_;
    DriverId device_driver_ = kDefaultDriver;

    /// The next DriverId to give out. 0 is kDefaultDriver, never given out.
    std::uint32_t next_driver_ = 1;

    /// The format of the last device that opened.
    std::uint32_t sample_rate_ = 0;
    std::uint32_t channels_ = 0;

    Clock::time_point device_started_;
    Clock::time_point last_progress_;
    std::uint64_t last_frames_ = 0;
    Clock::time_point last_retry_;
};

} // namespace ks::engine
