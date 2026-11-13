// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/miniaudio_backend.h>

#include <ks/core/clock.h>
#include <ks/core/realtime.h>
#include <ks/engine/block.h>
#include <ks/engine/interleave.h>

#include <miniaudio.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ks::engine {

namespace {

std::unexpected<BackendError> failure(BackendError::Kind kind, std::string message) {
    return std::unexpected{BackendError{.kind = kind, .message = std::move(message)}};
}

std::string too_many_channels(std::uint32_t channels) {
    return std::format("{} channels: the engine renders at most {}", channels, kMaxOutputChannels);
}

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
            ma_device_uninit(&device_); // stops the device first
        }
        if (context_open_) {
            static_cast<void>(ma_context_uninit(&context_));
        }
    }

    std::expected<void, BackendError> open(const MiniaudioConfig& config) {
        if (config.channels > kMaxOutputChannels) {
            return failure(BackendError::Kind::TooManyChannels, too_many_channels(config.channels));
        }
        driver_ = config.driver;

        // Stub (specs/miniaudio-devices/spec.md requirements 2 and 6): only
        // System and Null are told apart, and config.device is ignored.
        std::array<ma_backend, 1> null_backend{ma_backend_null};
        const bool use_null = config.backends == MiniaudioBackends::Null;
        if (const ma_result result = ma_context_init(use_null ? null_backend.data() : nullptr,
                                                     use_null ? 1 : 0, nullptr, &context_);
            result != MA_SUCCESS) {
            return failure(BackendError::Kind::NoBackend,
                           std::format("no audio backend could be initialized: {}",
                                       ma_result_description(result)));
        }
        context_open_ = true;

        ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
        device_config.playback.format = ma_format_f32;
        device_config.playback.channels = config.channels;       // 0: the device's own
        device_config.sampleRate = config.sample_rate;           // 0: the device's own
        device_config.periodSizeInFrames = config.period_frames; // 0: the backend's choice
        device_config.performanceProfile = ma_performance_profile_low_latency;
        // Call back with whatever the backend delivers; the engine splits it
        // into blocks itself (ADR 0003 §3).
        device_config.noFixedSizedCallback = MA_TRUE;
        device_config.dataCallback = &MiniaudioBackend::callback;
        device_config.pUserData = this;

        if (const ma_result result = ma_device_init(&context_, &device_config, &device_);
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
        info_ = StreamInfo{
            .backend = ma_get_backend_name(context_.backend),
            .device = std::string{std::data(device_.playback.name)},
            .sample_rate = device_.sampleRate,
            .channels = device_.playback.channels,
            .period_frames = device_.playback.internalPeriodSizeInFrames,
            // Stub (specs/miniaudio-devices/spec.md requirement 7): no
            // periods or latency estimate yet.
        };
        return {};
    }

    std::expected<void, BackendError> start() override {
        if (const ma_result result = ma_device_start(&device_); result != MA_SUCCESS) {
            return failure(BackendError::Kind::StartFailed,
                           std::format("could not start the output device: {}",
                                       ma_result_description(result)));
        }
        return {};
    }

    void stop() noexcept override {
        // Fails only if the device isn't running, which is what was asked for.
        static_cast<void>(ma_device_stop(&device_));
    }

    [[nodiscard]] const StreamInfo& info() const noexcept override { return info_; }

    // Stub (specs/miniaudio-devices/spec.md requirement 11): miniaudio's
    // notifications aren't handled yet.
    [[nodiscard]] bool lost() const noexcept override { return false; }

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
        // Stub (specs/miniaudio-devices/spec.md requirement 8): blocks carry
        // no output latency yet.
        static_cast<void>(render_interleaved(*self->core_, self->scratch_, out, self->channels_,
                                             self->sample_rate_, host_time, self->driver_));
    }

    RenderCore* core_;
    std::vector<float> scratch_storage_;
    std::array<std::span<float>, kMaxOutputChannels> scratch_{};
    std::size_t channels_ = 0;
    std::uint32_t sample_rate_ = 0;
    DriverId driver_ = kDefaultDriver;
    std::atomic<std::uint64_t> callbacks_{0};
    ma_context context_{};
    ma_device device_{};
    bool context_open_ = false;
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

// Stub (specs/miniaudio-devices/spec.md requirement 3).
std::string_view backend_name(MiniaudioBackends /*backends*/) noexcept {
    return {};
}

// Stub (specs/miniaudio-devices/spec.md requirement 3).
std::optional<MiniaudioBackends> parse_backend(std::string_view /*name*/) noexcept {
    return std::nullopt;
}

// Stub (specs/miniaudio-devices/spec.md requirement 5).
std::expected<OutputDeviceList, BackendError> list_output_devices(MiniaudioBackends /*backends*/) {
    return failure(BackendError::Kind::OpenFailed, "not implemented");
}

// Stub (specs/miniaudio-devices/spec.md requirement 10): no event loses the
// device.
void LossDetector::on_event(DeviceEvent /*event*/) noexcept KS_NONBLOCKING {}

void LossDetector::stopping() noexcept KS_NONBLOCKING {
    stopping_.store(true, std::memory_order_release);
}

void LossDetector::starting() noexcept KS_NONBLOCKING {
    stopping_.store(false, std::memory_order_release);
}

bool LossDetector::lost() const noexcept KS_NONBLOCKING {
    return lost_.load(std::memory_order_acquire);
}

// Stub (specs/miniaudio-devices/spec.md requirements 12 to 15): the source
// can't be created yet.
class MiniaudioDeviceSource::Lister {};

std::expected<std::unique_ptr<MiniaudioDeviceSource>, BackendError>
MiniaudioDeviceSource::create(const MiniaudioSourceConfig& /*config*/) {
    return failure(BackendError::Kind::NoBackend, "not implemented");
}

MiniaudioDeviceSource::MiniaudioDeviceSource(Key /*key*/, const MiniaudioSourceConfig& config,
                                             std::unique_ptr<Lister> lister, OutputDevice device)
    : config_{config}, lister_{std::move(lister)}, device_{std::move(device)} {}

MiniaudioDeviceSource::~MiniaudioDeviceSource() = default;

bool MiniaudioDeviceSource::available() {
    return false;
}

std::expected<std::unique_ptr<AudioBackend>, BackendError>
MiniaudioDeviceSource::open(RenderCore& /*core*/, DriverId /*driver*/) {
    return failure(BackendError::Kind::OpenFailed, "not implemented");
}

const OutputDevice& MiniaudioDeviceSource::device() const noexcept {
    return device_;
}

} // namespace ks::engine
