// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/miniaudio_backend.h>

#include <ks/core/clock.h>
#include <ks/core/realtime.h>
#include <ks/engine/block.h>
#include <ks/engine/interleave.h>

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ks::engine {

namespace {

// A key holds a device ID's bytes (specs/miniaudio-devices/spec.md
// requirement 4).
static_assert(sizeof(ma_device_id) == kDeviceKeySize);
static_assert(std::is_trivially_copyable_v<ma_device_id>);

constexpr std::array kAllBackends{
    MiniaudioBackends::System, MiniaudioBackends::Null, MiniaudioBackends::PulseAudio,
    MiniaudioBackends::Alsa,   MiniaudioBackends::Jack, MiniaudioBackends::CoreAudio,
};

std::unexpected<BackendError> failure(BackendError::Kind kind, std::string message) {
    return std::unexpected{BackendError{.kind = kind, .message = std::move(message)}};
}

std::string too_many_channels(std::uint32_t channels) {
    return std::format("{} channels: the engine renders at most {}", channels, kMaxOutputChannels);
}

/// The one miniaudio backend that `backends` names; none for System, which
/// tries miniaudio's default order (requirement 2).
std::optional<ma_backend> only_backend(MiniaudioBackends backends) noexcept {
    switch (backends) {
    case MiniaudioBackends::System:
        return std::nullopt;
    case MiniaudioBackends::Null:
        return ma_backend_null;
    case MiniaudioBackends::PulseAudio:
        return ma_backend_pulseaudio;
    case MiniaudioBackends::Alsa:
        return ma_backend_alsa;
    case MiniaudioBackends::Jack:
        return ma_backend_jack;
    case MiniaudioBackends::CoreAudio:
        return ma_backend_coreaudio;
    }
    return std::nullopt;
}

DeviceKey key_of(const ma_device_id& id) noexcept {
    DeviceKey key;
    std::memcpy(key.bytes.data(), &id, kDeviceKeySize);
    return key;
}

ma_device_id id_of(const DeviceKey& key) noexcept {
    ma_device_id id{};
    std::memcpy(&id, key.bytes.data(), kDeviceKeySize);
    return id;
}

/// Whether one of `devices` has `key`.
bool has_key(std::span<const OutputDevice> devices, const DeviceKey& key) {
    return std::ranges::find(devices, key, &OutputDevice::key) != devices.end();
}

/// The event a miniaudio notification is; none for one it doesn't know
/// (requirement 9).
std::optional<DeviceEvent> device_event(ma_device_notification_type type) noexcept KS_NONBLOCKING {
    switch (type) {
    case ma_device_notification_type_started:
        return DeviceEvent::Started;
    case ma_device_notification_type_stopped:
        return DeviceEvent::Stopped;
    case ma_device_notification_type_rerouted:
        return DeviceEvent::Rerouted;
    case ma_device_notification_type_interruption_began:
        return DeviceEvent::InterruptionBegan;
    case ma_device_notification_type_interruption_ended:
        return DeviceEvent::InterruptionEnded;
    case ma_device_notification_type_unlocked:
        return DeviceEvent::Unlocked;
    }
    return std::nullopt;
}

/// A miniaudio context on a choice of backends, uninitialized when it goes.
/// It never moves: devices keep a pointer to it.
class Context {
public:
    Context() = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    ~Context() {
        if (open_) {
            static_cast<void>(ma_context_uninit(&context_));
        }
    }

    /// Initializes the first of `backends` that works. NoBackend if none
    /// does. Called once.
    std::expected<void, BackendError> init(MiniaudioBackends backends) {
        const std::optional<ma_backend> only = only_backend(backends);
        const std::array<ma_backend, 1> chosen{only.value_or(ma_backend_null)};
        if (const ma_result result =
                ma_context_init(only.has_value() ? chosen.data() : nullptr,
                                only.has_value() ? 1U : 0U, nullptr, &context_);
            result != MA_SUCCESS) {
            if (only.has_value()) {
                return failure(BackendError::Kind::NoBackend,
                               std::format("the {} audio backend could not be initialized: {}",
                                           backend_name(backends), ma_result_description(result)));
            }
            return failure(BackendError::Kind::NoBackend,
                           std::format("no audio backend could be initialized: {}",
                                       ma_result_description(result)));
        }
        open_ = true;
        return {};
    }

    [[nodiscard]] ma_context& get() noexcept { return context_; }

    /// miniaudio's name for the backend that initialized.
    [[nodiscard]] std::string backend() const { return ma_get_backend_name(context_.backend); }

    /// The playback devices, in miniaudio's order. OpenFailed if they
    /// couldn't be listed.
    std::expected<std::vector<OutputDevice>, BackendError> playback_devices() {
        ma_device_info* infos = nullptr;
        ma_uint32 count = 0;
        if (const ma_result result =
                ma_context_get_devices(&context_, &infos, &count, nullptr, nullptr);
            result != MA_SUCCESS) {
            return failure(BackendError::Kind::OpenFailed,
                           std::format("could not list the {} output devices: {}", backend(),
                                       ma_result_description(result)));
        }
        std::vector<OutputDevice> devices;
        devices.reserve(count);
        for (const ma_device_info& info : std::span{infos, std::size_t{count}}) {
            devices.push_back(OutputDevice{
                .name = std::string{std::data(info.name)},
                .key = key_of(info.id),
                .is_default = info.isDefault != MA_FALSE,
            });
        }
        return devices;
    }

private:
    ma_context context_{};
    bool open_ = false;
};

/// A playback device opened with miniaudio. It never moves: miniaudio keeps
/// pointers to its context and device.
class MiniaudioBackend final : public AudioBackend {
public:
    explicit MiniaudioBackend(RenderCore& core)
        : core_{&core}, scratch_storage_(kMaxOutputChannels * kMaxBlockFrames) {
        for (std::size_t c = 0; c < kMaxOutputChannels; ++c) {
            scratch_.at(c) =
                std::span{scratch_storage_}.subspan(c * kMaxBlockFrames, kMaxBlockFrames);
        }
    }

    MiniaudioBackend(const MiniaudioBackend&) = delete;
    MiniaudioBackend& operator=(const MiniaudioBackend&) = delete;
    MiniaudioBackend(MiniaudioBackend&&) = delete;
    MiniaudioBackend& operator=(MiniaudioBackend&&) = delete;

    ~MiniaudioBackend() override {
        if (device_open_) {
            loss_.stopping();           // uninitializing stops the device
            ma_device_uninit(&device_); // before context_ goes
        }
    }

    std::expected<void, BackendError> open(const MiniaudioConfig& config) {
        if (config.channels > kMaxOutputChannels) {
            return failure(BackendError::Kind::TooManyChannels, too_many_channels(config.channels));
        }
        driver_ = config.driver;

        if (auto initialized = context_.init(config.backends); !initialized) {
            return std::unexpected{std::move(initialized.error())};
        }

        // miniaudio opens whatever ID it's given on some backends, the null
        // backend among them, so the key is checked against the list here
        // (requirement 6).
        ma_device_id device_id{};
        const ma_device_id* chosen_id = nullptr; // the system default device
        if (config.device.has_value()) {
            const DeviceKey& key = *config.device;
            auto devices = context_.playback_devices();
            if (!devices) {
                return std::unexpected{std::move(devices.error())};
            }
            if (!has_key(*devices, key)) {
                return failure(
                    BackendError::Kind::OpenFailed,
                    std::format("no {} output device has the chosen key", context_.backend()));
            }
            device_id = id_of(key);
            chosen_id = &device_id;
        }

        ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
        device_config.playback.pDeviceID = chosen_id;
        device_config.playback.format = ma_format_f32;
        device_config.playback.channels = config.channels;       // 0: the device's own
        device_config.sampleRate = config.sample_rate;           // 0: the device's own
        device_config.periodSizeInFrames = config.period_frames; // 0: the backend's choice
        device_config.performanceProfile = ma_performance_profile_low_latency;
        // Call back with whatever the backend delivers; the engine splits it
        // into blocks itself (ADR 0003 §3).
        device_config.noFixedSizedCallback = MA_TRUE;
        device_config.dataCallback = &MiniaudioBackend::callback;
        device_config.notificationCallback = &MiniaudioBackend::notified;
        device_config.pUserData = this;

        if (const ma_result result = ma_device_init(&context_.get(), &device_config, &device_);
            result != MA_SUCCESS) {
            return failure(
                BackendError::Kind::OpenFailed,
                std::format("could not open the output device: {}", ma_result_description(result)));
        }
        device_open_ = true;

        if (device_.playback.channels > kMaxOutputChannels) {
            return failure(BackendError::Kind::TooManyChannels,
                           too_many_channels(device_.playback.channels));
        }
        channels_ = device_.playback.channels;
        sample_rate_ = device_.sampleRate;
        const std::uint32_t period_frames = device_.playback.internalPeriodSizeInFrames;
        const std::uint32_t periods = device_.playback.internalPeriods;
        // The whole buffer is ahead of the block being rendered
        // (requirement 7).
        output_latency_ =
            frames_to_duration(std::uint64_t{period_frames} * periods, device_.sampleRate);
        info_ = StreamInfo{
            .backend = context_.backend(),
            .device = std::string{std::data(device_.playback.name)},
            .sample_rate = device_.sampleRate,
            .channels = device_.playback.channels,
            .period_frames = period_frames,
            .periods = periods,
            .output_latency = output_latency_,
        };
        return {};
    }

    std::expected<void, BackendError> start() override {
        loss_.starting();
        if (const ma_result result = ma_device_start(&device_); result != MA_SUCCESS) {
            return failure(BackendError::Kind::StartFailed,
                           std::format("could not start the output device: {}",
                                       ma_result_description(result)));
        }
        return {};
    }

    void stop() noexcept override {
        // Some backends report stopping the device, before ma_device_stop()
        // returns; that isn't a loss (requirement 11).
        loss_.stopping();
        // Fails only if the device isn't running, which is what was asked for.
        static_cast<void>(ma_device_stop(&device_));
    }

    [[nodiscard]] const StreamInfo& info() const noexcept override { return info_; }

    [[nodiscard]] bool lost() const noexcept override { return loss_.lost(); }

    [[nodiscard]] std::uint64_t callbacks() const noexcept override {
        return callbacks_.load(std::memory_order_relaxed);
    }

private:
    // The audio thread's entry point.
    static void callback(ma_device* device, void* output, const void* /*input*/,
                         ma_uint32 frames) noexcept KS_NONBLOCKING {
        const auto host_time = core::monotonic_now();
        auto* const self = static_cast<MiniaudioBackend*>(device->pUserData);
        self->callbacks_.fetch_add(1, std::memory_order_relaxed);
        const std::span<float> out{static_cast<float*>(output),
                                   std::size_t{frames} * self->channels_};
        static_cast<void>(render_interleaved(*self->core_, self->scratch_, out, self->channels_,
                                             self->sample_rate_, host_time, self->driver_,
                                             self->output_latency_));
    }

    // miniaudio's notifications, from whichever thread it sends them on,
    // the audio thread among them (requirement 17).
    static void notified(const ma_device_notification* notification) noexcept KS_NONBLOCKING {
        auto* const self = static_cast<MiniaudioBackend*>(notification->pDevice->pUserData);
        if (const std::optional<DeviceEvent> event = device_event(notification->type)) {
            self->loss_.on_event(*event);
        }
    }

    RenderCore* core_;
    std::vector<float> scratch_storage_;
    std::array<std::span<float>, kMaxOutputChannels> scratch_{};
    std::size_t channels_ = 0;
    std::uint32_t sample_rate_ = 0;
    DriverId driver_ = kDefaultDriver;
    std::chrono::nanoseconds output_latency_{0};
    std::atomic<std::uint64_t> callbacks_{0};
    LossDetector loss_;
    Context context_;
    ma_device device_{};
    bool device_open_ = false;
    StreamInfo info_;
};

} // namespace

std::expected<std::unique_ptr<AudioBackend>, BackendError>
open_miniaudio(RenderCore& core, const MiniaudioConfig& config) {
    auto backend = std::make_unique<MiniaudioBackend>(core);
    if (auto opened = backend->open(config); !opened) {
        return std::unexpected{std::move(opened.error())};
    }
    return backend;
}

std::string_view backend_name(MiniaudioBackends backends) noexcept {
    switch (backends) {
    case MiniaudioBackends::System:
        return "system";
    case MiniaudioBackends::Null:
        return "null";
    case MiniaudioBackends::PulseAudio:
        return "pulseaudio";
    case MiniaudioBackends::Alsa:
        return "alsa";
    case MiniaudioBackends::Jack:
        return "jack";
    case MiniaudioBackends::CoreAudio:
        return "coreaudio";
    }
    return {};
}

std::optional<MiniaudioBackends> parse_backend(std::string_view name) noexcept {
    for (const MiniaudioBackends backends : kAllBackends) {
        if (backend_name(backends) == name) {
            return backends;
        }
    }
    return std::nullopt;
}

std::expected<OutputDeviceList, BackendError> list_output_devices(MiniaudioBackends backends) {
    Context context;
    if (auto initialized = context.init(backends); !initialized) {
        return std::unexpected{std::move(initialized.error())};
    }
    auto devices = context.playback_devices();
    if (!devices) {
        return std::unexpected{std::move(devices.error())};
    }
    return OutputDeviceList{.backend = context.backend(), .devices = std::move(*devices)};
}

void LossDetector::on_event(DeviceEvent event) noexcept KS_NONBLOCKING {
    switch (event) {
    case DeviceEvent::Stopped:
        if (!stopping_.load(std::memory_order_acquire)) {
            lost_.store(true, std::memory_order_release);
        }
        return;
    case DeviceEvent::Rerouted:
    case DeviceEvent::InterruptionBegan:
        lost_.store(true, std::memory_order_release);
        return;
    case DeviceEvent::Started:
    case DeviceEvent::InterruptionEnded:
    case DeviceEvent::Unlocked:
        return;
    }
}

void LossDetector::stopping() noexcept KS_NONBLOCKING {
    stopping_.store(true, std::memory_order_release);
}

void LossDetector::starting() noexcept KS_NONBLOCKING {
    stopping_.store(false, std::memory_order_release);
}

bool LossDetector::lost() const noexcept KS_NONBLOCKING {
    return lost_.load(std::memory_order_acquire);
}

/// The context a source lists devices through, which it keeps
/// (requirement 13).
class MiniaudioDeviceSource::Lister final : public Context {};

std::expected<std::unique_ptr<MiniaudioDeviceSource>, BackendError>
MiniaudioDeviceSource::create(const MiniaudioSourceConfig& config) {
    auto lister = std::make_unique<Lister>();
    if (auto initialized = lister->init(config.backends); !initialized) {
        return std::unexpected{std::move(initialized.error())};
    }
    auto devices = lister->playback_devices();

    // Resolve the device (requirement 12).
    OutputDevice device;
    if (config.device.has_value()) {
        // The key, whether or not a device has it now; with its name if one
        // does.
        device.key = *config.device;
        if (devices.has_value()) {
            if (const auto found = std::ranges::find(*devices, device.key, &OutputDevice::key);
                found != devices->end()) {
                device = *found;
            }
        }
    } else {
        if (!devices) {
            return std::unexpected{std::move(devices.error())};
        }
        if (devices->empty()) {
            return failure(BackendError::Kind::OpenFailed,
                           std::format("{} has no output devices", lister->backend()));
        }
        const auto marked = std::ranges::find(*devices, true, &OutputDevice::is_default);
        device = marked != devices->end() ? *marked : devices->front();
    }
    return std::make_unique<MiniaudioDeviceSource>(Key{}, config, std::move(lister),
                                                   std::move(device));
}

MiniaudioDeviceSource::MiniaudioDeviceSource(Key /*key*/, const MiniaudioSourceConfig& config,
                                             std::unique_ptr<Lister> lister, OutputDevice device)
    : config_{config}, lister_{std::move(lister)}, device_{std::move(device)} {}

MiniaudioDeviceSource::~MiniaudioDeviceSource() = default;

bool MiniaudioDeviceSource::available() {
    const auto devices = lister_->playback_devices();
    return devices.has_value() && has_key(*devices, device_.key);
}

std::expected<std::unique_ptr<AudioBackend>, BackendError>
MiniaudioDeviceSource::open(RenderCore& core, DriverId driver) {
    if (!available()) {
        if (device_.name.empty()) {
            return failure(BackendError::Kind::OpenFailed,
                           "the chosen output device isn't available");
        }
        return failure(BackendError::Kind::OpenFailed,
                       std::format("the output device {} isn't available", device_.name));
    }
    // After the first open, ask for its format, so a device that returns
    // comes back in the same one (requirement 14).
    const bool pinned = opened_sample_rate_ != 0;
    const MiniaudioConfig open_config{
        .backends = config_.backends,
        .channels = pinned ? opened_channels_ : config_.channels,
        .sample_rate = pinned ? opened_sample_rate_ : config_.sample_rate,
        .period_frames = config_.period_frames,
        .driver = driver,
        .device = device_.key,
    };
    auto opened = open_miniaudio(core, open_config);
    if (opened.has_value() && !pinned) {
        opened_sample_rate_ = (*opened)->info().sample_rate;
        opened_channels_ = (*opened)->info().channels;
    }
    return opened;
}

const OutputDevice& MiniaudioDeviceSource::device() const noexcept {
    return device_;
}

} // namespace ks::engine
