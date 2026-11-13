// SPDX-License-Identifier: GPL-3.0-or-later
// For the [manual] cable-pull criteria of specs/miniaudio-devices/spec.md.
// Hidden, so it runs only when asked for:
//
//   KS_CABLE_BACKEND=pulseaudio KS_CABLE_DEVICE=2 \
//       ks_miniaudio_devices_acceptance_tests "[cable]"
//
// KS_CABLE_BACKEND: a backend name (default "system"). KS_CABLE_DEVICE: a
// device number from the list it prints (default: the default device).
// KS_CABLE_SECONDS: how long to play (default 60). Pull the cable while it
// plays, then plug it back in, and watch the state changes it prints.

#include <ks/core/realtime.h>
#include <ks/engine/audio_backend.h>
#include <ks/engine/block.h>
#include <ks/engine/miniaudio_backend.h>
#include <ks/engine/output_controller.h>
#include <ks/engine/render_core.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

using ks::engine::backend_name;
using ks::engine::BackendError;
using ks::engine::Block;
using ks::engine::DeviceKey;
using ks::engine::Graph;
using ks::engine::list_output_devices;
using ks::engine::MiniaudioBackends;
using ks::engine::MiniaudioDeviceSource;
using ks::engine::MiniaudioSourceConfig;
using ks::engine::OutputConfig;
using ks::engine::OutputController;
using ks::engine::OutputDevice;
using ks::engine::OutputDeviceList;
using ks::engine::OutputState;
using ks::engine::parse_backend;
using ks::engine::RenderCore;
using ks::engine::test::Clock;
using ks::engine::test::error_text;
using ks::engine::test::TimePoint;

using std::chrono::milliseconds;

/// A sine wave on every channel, at the device's rate. It fades out over
/// 10 ms once asked, so stopping doesn't click.
class ToneGraph final : public Graph {
public:
    ToneGraph(double frequency_hz, float amplitude) noexcept
        : frequency_hz_{frequency_hz}, amplitude_{amplitude} {}

    void fade_out() noexcept { fading_.store(true, std::memory_order_relaxed); }

    void process(const Block& block) noexcept KS_NONBLOCKING override {
        const double increment =
            2.0 * std::numbers::pi * frequency_hz_ / static_cast<double>(block.sample_rate);
        const bool fading = fading_.load(std::memory_order_relaxed);
        const float fade_step = 100.0F / static_cast<float>(block.sample_rate); // 10 ms
        for (std::size_t i = 0; i < block.frames; ++i) {
            if (fading) {
                gain_ = std::max(0.0F, gain_ - fade_step);
            }
            const float sample = gain_ * amplitude_ * static_cast<float>(std::sin(phase_));
            for (const std::span<float> channel : block.channels) {
                channel[i] = sample;
            }
            phase_ += increment;
            if (phase_ >= 2.0 * std::numbers::pi) {
                phase_ -= 2.0 * std::numbers::pi;
            }
        }
    }

private:
    double frequency_hz_;
    float amplitude_;
    float gain_ = 1.0F;
    double phase_ = 0.0;
    std::atomic<bool> fading_{false};
};

std::optional<std::string_view> environment(const char* name) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing in the tests sets the environment
    const char* const value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string_view{value};
}

/// A non-negative integer from the environment, or `fallback` if unset.
/// Nothing if it's set but isn't one.
std::optional<std::size_t> number(const char* name, std::optional<std::size_t> fallback) {
    const std::optional<std::string_view> text = environment(name);
    if (!text.has_value()) {
        return fallback;
    }
    const char* const first = text->data();
    const char* const last = std::to_address(text->end());
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(first, last, value);
    if (text->empty() || error != std::errc{} || end != last) {
        return std::nullopt;
    }
    return value;
}

std::string_view state_name(OutputState state) {
    switch (state) {
    case OutputState::Stopped:
        return "Stopped";
    case OutputState::Playing:
        return "Playing";
    case OutputState::Freewheeling:
        return "Freewheeling";
    case OutputState::Attaching:
        return "Attaching";
    }
    return "?";
}

} // namespace

TEST_CASE("Cable: a tone through a chosen device", "[.][cable]") {
    const std::optional<MiniaudioBackends> backends =
        parse_backend(environment("KS_CABLE_BACKEND").value_or("system"));
    REQUIRE(backends.has_value());
    const MiniaudioBackends chosen = backends.value_or(MiniaudioBackends::System);

    const std::expected<OutputDeviceList, BackendError> listed = list_output_devices(chosen);
    CAPTURE(error_text(listed));
    REQUIRE(listed.has_value());
    std::cout << std::format("Output devices on {} ({}):\n", listed->backend, backend_name(chosen));
    std::size_t index = 0;
    for (const OutputDevice& device : listed->devices) {
        std::cout << std::format("  {}: {}{}\n", index, device.name,
                                 device.is_default ? " (default)" : "");
        ++index;
    }

    const std::optional<std::size_t> device_number = number("KS_CABLE_DEVICE", std::nullopt);
    std::optional<DeviceKey> key;
    if (environment("KS_CABLE_DEVICE").has_value()) {
        REQUIRE(device_number.has_value());
        REQUIRE(device_number.value_or(0) < listed->devices.size());
        key = std::span{listed->devices}[device_number.value_or(0)].key;
    }
    const std::optional<std::size_t> seconds = number("KS_CABLE_SECONDS", 60);
    REQUIRE(seconds.has_value());

    ToneGraph graph{440.0, 0.1F}; // -20 dBFS
    RenderCore core{graph};
    const auto created =
        MiniaudioDeviceSource::create(MiniaudioSourceConfig{.backends = chosen, .device = key});
    CAPTURE(error_text(created));
    REQUIRE(created.has_value());
    std::cout << std::format("Playing 440 Hz at -20 dBFS on {} for {} s. Pull the cable, then "
                             "plug it back in.\n",
                             (*created)->device().name, seconds.value_or(0))
              << std::flush;

    OutputController controller{core, **created, OutputConfig{}};
    const TimePoint started = Clock::now();
    controller.start();
    OutputState shown = controller.state();
    std::cout << std::format("  0.000 s  {}\n", state_name(shown)) << std::flush;

    const TimePoint end = started + std::chrono::seconds{seconds.value_or(0)};
    while (Clock::now() < end) {
        controller.poll();
        if (controller.state() != shown) {
            shown = controller.state();
            const double at = std::chrono::duration<double>(Clock::now() - started).count();
            std::cout << std::format("{:7.3f} s  {}\n", at, state_name(shown)) << std::flush;
        }
        std::this_thread::sleep_for(milliseconds{5});
    }

    graph.fade_out();
    for (int i = 0; i < 20; ++i) {
        controller.poll();
        std::this_thread::sleep_for(milliseconds{5});
    }
    controller.stop();
    REQUIRE(controller.state() == OutputState::Stopped);
}
