// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Keeping the core rendering through device loss (ADR 0002 §4, ADR 0003 §15).

#include <ks/engine/audio_backend.h>
#include <ks/engine/paced_backend.h>
#include <ks/engine/render_core.h>
#include <ks/engine/thread_priority.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

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
    /// Once a device's thread has called back, raises it to real-time
    /// priority if it isn't already (specs/health-checks/spec.md
    /// requirement 5).
    void poll();

    /// Mutes the core, waits for the fade out (at most the fade time plus
    /// 100 ms), then stops and closes whatever is running.
    void stop();

    [[nodiscard]] OutputState state() const noexcept;

    /// The sample rate and channel count of the last device that opened, or
    /// 0 if none has. While Playing, that's the device playing.
    [[nodiscard]] std::uint32_t device_sample_rate() const noexcept;
    [[nodiscard]] std::uint32_t device_channels() const noexcept;

    /// The scheduling of the active driver's thread, after asking for
    /// real-time priority (requirement 6): the device's while Playing, the
    /// freewheel driver's while Freewheeling or Attaching. None while
    /// Stopped, or before the thread is known.
    [[nodiscard]] std::optional<ThreadScheduling> driver_scheduling() const noexcept;

    /// How many devices have opened and started so far.
    [[nodiscard]] std::uint64_t devices_started() const noexcept;

private:
    using Clock = std::chrono::steady_clock;

    /// poll()'s work on the state.
    void poll_state();

    /// A DriverId never given out before.
    DriverId next_driver() noexcept;

    /// Changes the state, and the core's output state with it.
    void set_state(OutputState state) noexcept;

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

    /// Makes `device`, which has just started, the device waiting to become
    /// active or to be adopted; its thread isn't known yet.
    void set_device(std::unique_ptr<AudioBackend> device) noexcept;

    /// Raises the device's thread, and reads the freewheel driver's, once
    /// they're known.
    void check_threads() noexcept;

    /// Stops and destroys `backend`, if there is one.
    template <typename Backend>
    static void close(std::unique_ptr<Backend>& backend) noexcept;

    RenderCore* core_;
    DeviceSource* source_;
    OutputConfig config_;
    OutputState state_ = OutputState::Stopped;

    /// While Playing, the active device; while Attaching, the device waiting
    /// to become active.
    std::unique_ptr<AudioBackend> device_;
    std::unique_ptr<PacedBackend> freewheel_;
    DriverId device_driver_ = kDefaultDriver;

    /// The device's thread's scheduling, once it has been raised; and the
    /// freewheel driver's, once it has asked. Cleared when they go, so they
    /// never name a thread that has ended.
    std::optional<ThreadScheduling> device_scheduling_;
    std::optional<ThreadScheduling> freewheel_scheduling_;
    std::uint64_t devices_started_ = 0;

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
