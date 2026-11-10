// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The paced backend: renders in real time on its own thread, as a device
/// would (ADR 0003 §1). It stands in for a device in tests, and, asking for
/// real-time priority without recording, it's the freewheel driver.

#include <ks/core/realtime.h>
#include <ks/engine/audio_backend.h>
#include <ks/engine/render_core.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>

namespace ks::engine {

/// How a simulated device goes away.
enum class Loss : std::uint8_t {
    /// Callbacks just stop.
    Silent,
    /// Callbacks stop, and the backend reports it through lost().
    Reported,
};

/// A device that tests can unplug and plug back in. Paced backends playing on
/// it stop when it's unplugged, and stay stopped: like a real device's
/// stream, it must be opened again. It must outlive the backends playing on
/// it. Any thread may call its functions.
class SimulatedDevice {
public:
    SimulatedDevice() = default;
    SimulatedDevice(const SimulatedDevice&) = delete;
    SimulatedDevice& operator=(const SimulatedDevice&) = delete;
    SimulatedDevice(SimulatedDevice&&) = delete;
    SimulatedDevice& operator=(SimulatedDevice&&) = delete;
    ~SimulatedDevice() = default;

    void unplug(Loss loss) noexcept;
    void plug_in() noexcept;
    [[nodiscard]] bool present() const noexcept KS_NONBLOCKING;

    /// How many times it has been unplugged. A backend renders only while
    /// this is what it was when the backend was opened.
    [[nodiscard]] std::uint64_t unplugs() const noexcept KS_NONBLOCKING;

    /// How it was last unplugged.
    [[nodiscard]] Loss last_loss() const noexcept KS_NONBLOCKING;

private:
    std::atomic<bool> present_{true};
    std::atomic<std::uint64_t> unplugs_{0};
    std::atomic<Loss> last_loss_{Loss::Silent};
};

/// Whether a paced backend's thread got the real-time priority it asked for.
enum class Priority : std::uint8_t {
    NotRequested,
    Granted,
    /// It runs at its normal priority instead.
    Refused,
};

struct PacedConfig {
    std::uint32_t sample_rate = 48'000;
    std::uint32_t channels = 2;
    /// Frames per period. Periods larger than kMaxBlockFrames are rendered in
    /// several blocks.
    std::size_t period_frames = 256;
    /// The driver it renders as.
    DriverId driver = kDefaultDriver;
    /// How many frames of output to record. 0: none.
    std::size_t record_frames = 0;
    /// Ask for real-time priority: SCHED_FIFO at 70 on Linux, a
    /// time-constraint policy on macOS.
    bool realtime_priority = false;
    /// The host time of the first period, for the first start. If it's in
    /// the past, the backend catches up. Default: when start() is called.
    std::optional<std::chrono::steady_clock::time_point> start_time = std::nullopt;
    /// The simulated device it plays on, if any. Without one, it's never
    /// unplugged.
    SimulatedDevice* device = nullptr;
};

/// A paced backend. info() reports backend "Paced".
class PacedBackend : public AudioBackend {
public:
    /// What it recorded, interleaved: each period's output, silent periods
    /// included, up to the frames asked for. Read it once stop() has returned.
    [[nodiscard]] virtual std::span<const float> recording() const noexcept = 0;

    /// Whether its thread got real-time priority. Valid once it has started.
    [[nodiscard]] virtual Priority priority() const noexcept = 0;
};

/// Opens a paced backend rendering through `core`, which must outlive it. It
/// isn't started. Everything its thread uses is allocated here.
std::expected<std::unique_ptr<PacedBackend>, BackendError> open_paced(RenderCore& core,
                                                                      const PacedConfig& config);

/// Opens paced backends on a simulated device, for the output controller.
class PacedDeviceSource final : public DeviceSource {
public:
    /// Each backend it opens uses `config`, with the device and the driver
    /// replaced. The device must outlive the source.
    PacedDeviceSource(SimulatedDevice& device, const PacedConfig& config) noexcept;

    /// Whether the device is plugged in.
    [[nodiscard]] bool available() override;

    /// Opens a paced backend on the device, or fails with OpenFailed if it's
    /// unplugged.
    std::expected<std::unique_ptr<AudioBackend>, BackendError> open(RenderCore& core,
                                                                    DriverId driver) override;

private:
    SimulatedDevice* device_;
    PacedConfig config_;
};

} // namespace ks::engine
