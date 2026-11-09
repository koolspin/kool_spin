// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/tone/tone_player.h>

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <thread>
#include <utility>

namespace ks::tone {
namespace {

struct Backend {
    std::string_view name;
    ma_backend id;
};

constexpr std::array kBackends{
    Backend{.name = "pulseaudio", .id = ma_backend_pulseaudio},
    Backend{.name = "alsa", .id = ma_backend_alsa},
    Backend{.name = "jack", .id = ma_backend_jack},
    Backend{.name = "coreaudio", .id = ma_backend_coreaudio},
    Backend{.name = "null", .id = ma_backend_null},
};

constexpr auto kBackendNames = [] {
    std::array<std::string_view, kBackends.size()> names{};
    std::ranges::transform(kBackends, names.begin(), &Backend::name);
    return names;
}();

std::optional<ma_backend> find_backend(std::string_view name) {
    const auto* const found = std::ranges::find(kBackends, name, &Backend::name);
    if (found == kBackends.end()) {
        return std::nullopt;
    }
    return found->id;
}

/// Initializes `context` for `backend`, or for the first backend that works
/// when `backend` is empty. play_tone() and list_devices() both use this, so
/// they pick the same backend. On success, the caller uninitializes `context`.
std::expected<void, std::string> open_context(std::string_view backend, ma_context& context) {
    std::optional<ma_backend> requested;
    if (!backend.empty()) {
        requested = find_backend(backend);
        if (!requested) {
            return std::unexpected{std::format("unknown backend '{}'", backend)};
        }
    }

    const ma_uint32 backend_count = requested ? 1 : 0;
    if (ma_context_init(requested ? &*requested : nullptr, backend_count, nullptr, &context) !=
        MA_SUCCESS) {
        return std::unexpected{std::format("could not initialize the {} audio backend",
                                           backend.empty() ? "default" : backend)};
    }
    return {};
}

/// Runs a callable when the scope exits.
template <typename F>
class ScopeExit {
public:
    explicit ScopeExit(F f) noexcept : f_{std::move(f)} {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit(ScopeExit&&) = delete;
    ScopeExit& operator=(ScopeExit&&) = delete;
    ~ScopeExit() { f_(); }

private:
    F f_;
};

/// miniaudio's audio callback, called on its audio thread.
void render_callback(ma_device* device, void* output, const void* /*input*/,
                     ma_uint32 frame_count) noexcept KS_NONBLOCKING {
    auto* const generator = static_cast<ToneGenerator*>(device->pUserData);
    const std::size_t samples = std::size_t{frame_count} * device->playback.channels;
    generator->render(std::span{static_cast<float*>(output), samples});
}

} // namespace

std::span<const std::string_view> backend_names() noexcept {
    return kBackendNames;
}

std::optional<std::string> check_frequency(double frequency_hz, std::uint32_t sample_rate) {
    if (frequency_hz < static_cast<double>(sample_rate) / 2.0) {
        return std::nullopt;
    }
    return std::format("--frequency {} Hz is too high for the device's {} Hz sample rate",
                       frequency_hz, sample_rate);
}

std::expected<DeviceList, std::string> list_devices(std::string_view backend) {
    ma_context context;
    if (auto opened = open_context(backend, context); !opened) {
        return std::unexpected{std::move(opened.error())};
    }
    const ScopeExit uninit_context{[&context] { ma_context_uninit(&context); }};

    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (const ma_result result = ma_context_get_devices(&context, &infos, &count, nullptr, nullptr);
        result != MA_SUCCESS) {
        return std::unexpected{
            std::format("could not list the output devices: {}", ma_result_description(result))};
    }

    DeviceList list{.backend = ma_get_backend_name(context.backend), .devices = {}};
    const std::span playback{infos, std::size_t{count}};
    list.devices.reserve(playback.size());
    for (const ma_device_info& info : playback) {
        list.devices.push_back(OutputDevice{.name = std::string{std::data(info.name)},
                                            .is_default = info.isDefault != MA_FALSE});
    }
    return list;
}

std::expected<PlaybackReport, PlayError>
play_tone(std::string_view backend, const ToneSettings& settings,
          std::chrono::duration<double> duration, std::optional<std::uint32_t> device_index,
          const std::function<void(const DeviceInfo&)>& on_start) {
    const auto runtime_error = [](std::string message) {
        return std::unexpected{
            PlayError{.kind = PlayError::Kind::Runtime, .message = std::move(message)}};
    };

    ma_context context;
    if (auto opened = open_context(backend, context); !opened) {
        return runtime_error(std::move(opened.error()));
    }
    const ScopeExit uninit_context{[&context] { ma_context_uninit(&context); }};

    // The chosen device, copied out of the context's device list.
    std::optional<ma_device_id> device_id;
    if (device_index) {
        ma_device_info* infos = nullptr;
        ma_uint32 count = 0;
        if (const ma_result result =
                ma_context_get_devices(&context, &infos, &count, nullptr, nullptr);
            result != MA_SUCCESS) {
            return runtime_error(std::format("could not list the output devices: {}",
                                             ma_result_description(result)));
        }
        if (*device_index >= count) {
            return std::unexpected{PlayError{
                .kind = PlayError::Kind::Usage,
                .message = std::format("no output device {} on {}", *device_index,
                                       ma_get_backend_name(context.backend)),
            }};
        }
        device_id = std::span{infos, std::size_t{count}}[*device_index].id;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = 0; // the device's native rate, so nothing is resampled
    config.performanceProfile = ma_performance_profile_low_latency;
    config.dataCallback = render_callback;
    config.playback.pDeviceID = device_id ? &*device_id : nullptr; // null: the default device

    // Declared before the device so it is destroyed after the device is uninitialized.
    std::optional<ToneGenerator> generator;

    ma_device device;
    if (const ma_result result = ma_device_init(&context, &config, &device); result != MA_SUCCESS) {
        return runtime_error(
            std::format("could not open the output device: {}", ma_result_description(result)));
    }
    const ScopeExit uninit_device{[&device] { ma_device_uninit(&device); }};

    // Before the generator exists: its oscillator asserts the frequency is below Nyquist.
    if (auto too_high = check_frequency(settings.frequency_hz, device.sampleRate)) {
        return std::unexpected{
            PlayError{.kind = PlayError::Kind::Usage, .message = std::move(*too_high)}};
    }

    generator.emplace(settings, static_cast<double>(device.sampleRate), device.playback.channels);
    device.pUserData = &*generator;

    if (ma_device_start(&device) != MA_SUCCESS) {
        return runtime_error("could not start the output device");
    }

    const DeviceInfo info{
        .backend = ma_get_backend_name(context.backend),
        .device = std::string{std::data(device.playback.name)},
        .sample_rate = device.sampleRate,
        .channels = device.playback.channels,
        .period_frames = device.playback.internalPeriodSizeInFrames,
        .periods = device.playback.internalPeriods,
    };
    if (on_start) {
        on_start(info);
    }

    std::this_thread::sleep_for(duration);

    // Fade out, but don't hang if the device has stopped calling back.
    generator->request_stop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (!generator->finished() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    ma_device_stop(&device);

    return PlaybackReport{
        .device = info,
        .frames_rendered = generator->frames_rendered(),
        .peak = generator->peak(),
        .measured_hz = generator->measured_frequency_hz(),
    };
}

} // namespace ks::tone
