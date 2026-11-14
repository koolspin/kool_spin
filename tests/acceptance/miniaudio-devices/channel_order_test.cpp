// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/miniaudio-devices/spec.md requirement 18: the
// configuration a device is opened with asks miniaudio, and PulseAudio, to
// leave the engine's channels in order.

#include <ks/engine/block.h>
#include <ks/engine/miniaudio_backend.h>

#include "miniaudio_device_config.h"
#include <catch2/catch_test_macros.hpp>
#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace {

using ks::engine::backend_name;
using ks::engine::BlankChannelMap;
using ks::engine::kMaxOutputChannels;
using ks::engine::kPulseAlsaChannelMap;
using ks::engine::miniaudio_device_config;
using ks::engine::MiniaudioBackends;
using ks::engine::MiniaudioConfig;

constexpr std::array kAllBackends{
    MiniaudioBackends::System, MiniaudioBackends::Null, MiniaudioBackends::PulseAudio,
    MiniaudioBackends::Alsa,   MiniaudioBackends::Jack, MiniaudioBackends::CoreAudio,
};
constexpr std::array<std::uint32_t, 4> kChannelCounts{0, 2, 4, kMaxOutputChannels};
constexpr std::uint32_t kRate = 44'100;
constexpr std::uint32_t kPeriod = 128;

bool is_blank(const BlankChannelMap& map) {
    return std::ranges::all_of(map,
                               [](ma_channel position) { return position == MA_CHANNEL_NONE; });
}

/// Checks requirement 18's configuration for one choice of backends,
/// channel count, and device.
void check_config(MiniaudioBackends backends, std::uint32_t channels,
                  const ma_device_id* device_id) {
    CAPTURE(backend_name(backends), channels, device_id != nullptr);
    const MiniaudioConfig config{
        .backends = backends,
        .channels = channels,
        .sample_rate = kRate,
        .period_frames = kPeriod,
    };
    BlankChannelMap blank_map{};
    const ma_device_config device_config = miniaudio_device_config(config, device_id, blank_map);

    CHECK(device_config.deviceType == ma_device_type_playback);
    CHECK(device_config.playback.format == ma_format_f32);
    CHECK(device_config.playback.channels == channels);
    CHECK(device_config.sampleRate == kRate);
    CHECK(device_config.periodSizeInFrames == kPeriod);
    CHECK(device_config.playback.pDeviceID == device_id);
    CHECK(device_config.performanceProfile == ma_performance_profile_low_latency);
    CHECK(device_config.noFixedSizedCallback == MA_TRUE);

    CHECK(device_config.playback.pChannelMap == blank_map.data());
    CHECK(is_blank(blank_map));
    CHECK(device_config.pulse.channelMap == kPulseAlsaChannelMap);
}

/// A miniaudio context on the null backend, uninitialized when it goes.
class NullContext {
public:
    NullContext() {
        const ma_backend backend = ma_backend_null;
        initialized_ = ma_context_init(&backend, 1, nullptr, &context_) == MA_SUCCESS;
    }
    NullContext(const NullContext&) = delete;
    NullContext& operator=(const NullContext&) = delete;
    NullContext(NullContext&&) = delete;
    NullContext& operator=(NullContext&&) = delete;
    ~NullContext() {
        if (initialized_) {
            ma_context_uninit(&context_);
        }
    }

    [[nodiscard]] bool initialized() const { return initialized_; }
    [[nodiscard]] ma_context& get() { return context_; }

private:
    ma_context context_{};
    bool initialized_ = false;
};

} // namespace

TEST_CASE("Miniaudio: channels reach the device in order", "[miniaudio-devices]") {
    // PulseAudio's PA_CHANNEL_MAP_ALSA, part of its ABI. miniaudio.c checks
    // that MA_PA_CHANNEL_MAP_ALSA has the same value.
    STATIC_REQUIRE(kPulseAlsaChannelMap == 1);

    SECTION("the configuration") {
        ma_device_id some_device{};
        some_device.nullbackend = 1;
        for (const MiniaudioBackends backends : kAllBackends) {
            for (const std::uint32_t channels : kChannelCounts) {
                check_config(backends, channels, nullptr);
                check_config(backends, channels, &some_device);
            }
        }
    }

    SECTION("a null device opened with it converts no channels") {
        NullContext context;
        REQUIRE(context.initialized());
        const MiniaudioConfig config{
            .backends = MiniaudioBackends::Null,
            .channels = 4,
            .sample_rate = kRate,
            .period_frames = kPeriod,
        };
        BlankChannelMap blank_map{};
        const ma_device_config device_config = miniaudio_device_config(config, nullptr, blank_map);
        ma_device device{};
        REQUIRE(ma_device_init(&context.get(), &device_config, &device) == MA_SUCCESS);

        const std::uint32_t channels = device.playback.channels;
        const std::uint32_t internal_channels = device.playback.internalChannels;
        const std::span engine_side = std::span{device.playback.channelMap}.first(channels);
        const std::span device_side =
            std::span{device.playback.internalChannelMap}.first(internal_channels);
        const bool same_layout = std::ranges::equal(engine_side, device_side);
        const ma_channel_conversion_path path =
            device.playback.converter.channelConverter.conversionPath;
        ma_device_uninit(&device);

        CHECK(channels == 4);
        CHECK(internal_channels == 4);
        CHECK(same_layout);
        CHECK(path == ma_channel_conversion_path_passthrough);
        CHECK(is_blank(blank_map));
    }
}
