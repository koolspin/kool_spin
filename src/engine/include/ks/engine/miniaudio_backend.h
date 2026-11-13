// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Playing through a device with miniaudio: choosing a backend and a device,
/// noticing when the device goes, and reopening it when it returns
/// (specs/render-core/spec.md, specs/miniaudio-devices/spec.md).

#include <ks/core/realtime.h>
#include <ks/engine/audio_backend.h>
#include <ks/engine/render_core.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ks::engine {

/// Which of miniaudio's backends to try.
enum class MiniaudioBackends : std::uint8_t {
    /// The system's, in miniaudio's default order.
    System,
    /// miniaudio's null backend: one device, which calls back in real time
    /// without playing anything. For tests.
    Null,
    PulseAudio,
    Alsa,
    Jack,
    CoreAudio,
};

/// The name of `backends`: "system", "null", "pulseaudio", "alsa", "jack", or
/// "coreaudio" (specs/miniaudio-devices/spec.md requirement 3).
[[nodiscard]] std::string_view backend_name(MiniaudioBackends backends) noexcept;

/// The backends a name names, as backend_name() gives it; case-sensitive.
/// Nothing for any other string.
[[nodiscard]] std::optional<MiniaudioBackends> parse_backend(std::string_view name) noexcept;

/// The size of a DeviceKey: miniaudio's device ID.
inline constexpr std::size_t kDeviceKeySize = 256;

/// Identifies a device to the backend that listed it: the bytes of
/// miniaudio's device ID (requirement 4).
struct DeviceKey {
    std::array<std::byte, kDeviceKeySize> bytes{};

    bool operator==(const DeviceKey&) const = default;
};

/// A playback device, as a backend lists it.
struct OutputDevice {
    std::string name;
    DeviceKey key;
    /// The system's default playback device.
    bool is_default = false;
};

/// A backend's playback devices (requirement 5).
struct OutputDeviceList {
    /// miniaudio's name for the backend it used, for example "PulseAudio" or
    /// "Null".
    std::string backend;
    /// In miniaudio's order.
    std::vector<OutputDevice> devices;
};

/// Lists the playback devices of the first of `backends` that initializes.
/// NoBackend if none does; OpenFailed if the devices couldn't be listed.
[[nodiscard]] std::expected<OutputDeviceList, BackendError>
list_output_devices(MiniaudioBackends backends);

struct MiniaudioConfig {
    MiniaudioBackends backends = MiniaudioBackends::System;
    /// 0: the device's own channel count.
    std::uint32_t channels = 0;
    /// 0: the device's own sample rate.
    std::uint32_t sample_rate = 0;
    /// 0: the backend's choice.
    std::uint32_t period_frames = 0;
    /// The driver it renders as.
    DriverId driver = kDefaultDriver;
    /// The device to open, by its key; none for the system default device
    /// (requirement 6).
    std::optional<DeviceKey> device = std::nullopt;
};

/// Opens a playback device, rendering through `core`, which must outlive the
/// backend: the one `config.device` names, by its ID, or the system default
/// device. OpenFailed if no device has the key. The backend isn't started.
/// Everything its callback uses is allocated here. Every block it renders
/// carries info().output_latency.
std::expected<std::unique_ptr<AudioBackend>, BackendError>
open_miniaudio(RenderCore& core, const MiniaudioConfig& config);

/// miniaudio's notifications about a device (requirement 9).
enum class DeviceEvent : std::uint8_t {
    Started,
    Stopped,
    Rerouted,
    InterruptionBegan,
    InterruptionEnded,
    Unlocked,
};

/// Decides from a device's events whether it has gone (requirement 10). Any
/// thread may call any method: miniaudio can notify from the audio thread.
class LossDetector {
public:
    /// The device is lost on Stopped, unless a stop is in progress, and on
    /// Rerouted or InterruptionBegan. Other events change nothing.
    void on_event(DeviceEvent event) noexcept KS_NONBLOCKING;

    /// Marks a stop as in progress: the application is stopping the device.
    void stopping() noexcept KS_NONBLOCKING;

    /// Ends the stop in progress: the application is starting the device.
    void starting() noexcept KS_NONBLOCKING;

    /// Whether the device has been lost. Once lost, it stays lost.
    [[nodiscard]] bool lost() const noexcept KS_NONBLOCKING;

private:
    std::atomic<bool> lost_{false};
    std::atomic<bool> stopping_{false};
};

struct MiniaudioSourceConfig {
    MiniaudioBackends backends = MiniaudioBackends::System;
    /// The device, by its key; none to resolve the default device when the
    /// source is created.
    std::optional<DeviceKey> device = std::nullopt;
    /// 0: the device's own channel count.
    std::uint32_t channels = 0;
    /// 0: the device's own sample rate.
    std::uint32_t sample_rate = 0;
    /// 0: the backend's choice.
    std::uint32_t period_frames = 0;
};

/// Opens one device with miniaudio, and recognises it when it returns
/// (requirements 12 to 15). Used from the control thread.
class MiniaudioDeviceSource final : public DeviceSource {
    class Lister;
    /// Lets create() use the constructor, and nothing else.
    struct Key {
        explicit Key() = default;
    };

public:
    /// Initializes the backend and resolves the device: the configured key,
    /// whether or not a device has it now, or else the listed default device,
    /// or else the first listed. NoBackend if no backend initializes;
    /// OpenFailed with no key and no devices.
    [[nodiscard]] static std::expected<std::unique_ptr<MiniaudioDeviceSource>, BackendError>
    create(const MiniaudioSourceConfig& config);

    MiniaudioDeviceSource(Key key, const MiniaudioSourceConfig& config,
                          std::unique_ptr<Lister> lister, OutputDevice device);

    MiniaudioDeviceSource(const MiniaudioDeviceSource&) = delete;
    MiniaudioDeviceSource& operator=(const MiniaudioDeviceSource&) = delete;
    MiniaudioDeviceSource(MiniaudioDeviceSource&&) = delete;
    MiniaudioDeviceSource& operator=(MiniaudioDeviceSource&&) = delete;
    ~MiniaudioDeviceSource() override;

    /// Whether a listed device has the resolved key. False if listing fails.
    [[nodiscard]] bool available() override;

    /// Opens the resolved device by its key, not started, to render through
    /// `core` as `driver`: at the configured format until a device has opened
    /// through this source, and at the first one's format after that.
    /// OpenFailed, opening nothing, if the device isn't available.
    std::expected<std::unique_ptr<AudioBackend>, BackendError> open(RenderCore& core,
                                                                    DriverId driver) override;

    /// The resolved device. Its name is empty if a configured key wasn't
    /// listed when the source was created.
    [[nodiscard]] const OutputDevice& device() const noexcept;

private:
    MiniaudioSourceConfig config_;
    /// Holds the miniaudio context that available() lists devices with.
    std::unique_ptr<Lister> lister_;
    OutputDevice device_;
    /// The format the first device opened through this source had; 0 until
    /// one has.
    std::uint32_t opened_sample_rate_ = 0;
    std::uint32_t opened_channels_ = 0;
};

} // namespace ks::engine
