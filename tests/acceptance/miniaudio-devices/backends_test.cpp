// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/miniaudio-devices/spec.md: backends by name,
// listing devices, opening one by key, the latency estimate, and stopping
// without a loss.

#include <ks/engine/audio_backend.h>
#include <ks/engine/block.h>
#include <ks/engine/miniaudio_backend.h>
#include <ks/engine/render_core.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>

namespace {

using ks::engine::AudioBackend;
using ks::engine::backend_name;
using ks::engine::BackendError;
using ks::engine::DeviceKey;
using ks::engine::frames_to_duration;
using ks::engine::list_output_devices;
using ks::engine::MiniaudioBackends;
using ks::engine::MiniaudioConfig;
using ks::engine::open_miniaudio;
using ks::engine::OutputDevice;
using ks::engine::OutputDeviceList;
using ks::engine::parse_backend;
using ks::engine::RenderCore;
using ks::engine::StreamInfo;
using ks::engine::test::Clock;
using ks::engine::test::error_text;
using ks::engine::test::flipped;
using ks::engine::test::LatencyGraph;
using ks::engine::test::SilentGraph;
using ks::engine::test::TimePoint;
using ks::engine::test::wait_until;

using std::chrono::milliseconds;
using Opened = std::expected<std::unique_ptr<AudioBackend>, BackendError>;
using Listed = std::expected<OutputDeviceList, BackendError>;

constexpr std::uint32_t kRate = 48'000;
constexpr std::uint32_t kPeriod = 256;

/// The null backend's one device's key.
DeviceKey null_device_key() {
    const Listed listed = list_output_devices(MiniaudioBackends::Null);
    CAPTURE(error_text(listed));
    REQUIRE(listed.has_value());
    REQUIRE(listed->devices.size() == 1);
    return listed->devices.front().key;
}

/// A null device at 48 kHz, stereo, with a 256-frame period.
MiniaudioConfig null_config(std::optional<DeviceKey> device = std::nullopt) {
    return MiniaudioConfig{
        .backends = MiniaudioBackends::Null,
        .channels = 2,
        .sample_rate = kRate,
        .period_frames = kPeriod,
        .device = device,
    };
}

/// Checks lost() every millisecond for `duration`. Returns whether it stayed
/// false throughout.
bool never_lost(const AudioBackend& backend, milliseconds duration) {
    bool lost = backend.lost();
    const TimePoint end = Clock::now() + duration;
    while (Clock::now() < end) {
        std::this_thread::sleep_for(milliseconds{1});
        lost = lost || backend.lost();
    }
    return !lost;
}

} // namespace

TEST_CASE("Miniaudio: backends by name", "[miniaudio-devices]") {
    struct Named {
        MiniaudioBackends backends;
        std::string_view name;
    };
    constexpr std::array kNamed{
        Named{.backends = MiniaudioBackends::System, .name = "system"},
        Named{.backends = MiniaudioBackends::Null, .name = "null"},
        Named{.backends = MiniaudioBackends::PulseAudio, .name = "pulseaudio"},
        Named{.backends = MiniaudioBackends::Alsa, .name = "alsa"},
        Named{.backends = MiniaudioBackends::Jack, .name = "jack"},
        Named{.backends = MiniaudioBackends::CoreAudio, .name = "coreaudio"},
    };
    for (const Named& named : kNamed) {
        CAPTURE(named.name);
        CHECK(backend_name(named.backends) == named.name);
        const std::optional<MiniaudioBackends> parsed = parse_backend(named.name);
        REQUIRE(parsed.has_value());
        CHECK(parsed.value_or(MiniaudioBackends::System) == named.backends);
    }
    CHECK(parse_backend("PulseAudio") == std::nullopt);
    CHECK(parse_backend("") == std::nullopt);
    CHECK(parse_backend("wasapi") == std::nullopt);
}

TEST_CASE("Miniaudio: the null backend's devices", "[miniaudio-devices]") {
    const Listed first = list_output_devices(MiniaudioBackends::Null);
    CAPTURE(error_text(first));
    REQUIRE(first.has_value());
    CHECK(first->backend == "Null");
    REQUIRE(first->devices.size() == 1);
    const OutputDevice& device = first->devices.front();
    CHECK(device.name == "NULL Playback Device");
    CHECK(device.is_default);

    const Listed again = list_output_devices(MiniaudioBackends::Null);
    REQUIRE(again.has_value());
    REQUIRE(again->devices.size() == 1);
    CHECK(again->devices.front().key == device.key);
}

TEST_CASE("Miniaudio: opening a device by key", "[miniaudio-devices]") {
    const DeviceKey key = null_device_key();

    SECTION("the null device's key opens it") {
        SilentGraph graph;
        RenderCore core{graph};
        const Opened opened = open_miniaudio(core, null_config(key));
        CAPTURE(error_text(opened));
        REQUIRE(opened.has_value());
        AudioBackend& backend = **opened;
        CHECK(backend.info().device == "NULL Playback Device");
        REQUIRE(backend.start().has_value());
        const bool rendered =
            wait_until([&core] { return core.frames_rendered() > 0; }, milliseconds{1000});
        backend.stop();
        REQUIRE(rendered);
    }

    SECTION("a key no device has is refused") {
        SilentGraph graph;
        RenderCore core{graph};
        const Opened opened = open_miniaudio(core, null_config(flipped(key, 0)));
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().kind == BackendError::Kind::OpenFailed);
    }
}

TEST_CASE("Miniaudio: blocks carry the output latency", "[miniaudio-devices]") {
    LatencyGraph graph;
    RenderCore core{graph};
    const Opened opened = open_miniaudio(core, null_config());
    CAPTURE(error_text(opened));
    REQUIRE(opened.has_value());
    AudioBackend& backend = **opened;

    const StreamInfo& info = backend.info();
    CAPTURE(info.period_frames, info.periods, info.sample_rate, info.output_latency.count());
    REQUIRE(info.periods >= 1);
    REQUIRE(info.output_latency > std::chrono::nanoseconds{0});
    REQUIRE(info.output_latency ==
            frames_to_duration(std::uint64_t{info.period_frames} * info.periods, info.sample_rate));

    REQUIRE(backend.start().has_value());
    std::this_thread::sleep_for(milliseconds{200});
    backend.stop();

    CAPTURE(graph.calls(), graph.smallest().count(), graph.largest().count());
    REQUIRE(graph.calls() > 0);
    CHECK(graph.smallest() == info.output_latency);
    CHECK(graph.largest() == info.output_latency);
}

TEST_CASE("Miniaudio: stopping the device isn't a loss", "[miniaudio-devices]") {
    SilentGraph graph;
    RenderCore core{graph};
    const Opened opened = open_miniaudio(core, null_config());
    CAPTURE(error_text(opened));
    REQUIRE(opened.has_value());
    AudioBackend& backend = **opened;
    CHECK(never_lost(backend, milliseconds{0}));

    REQUIRE(backend.start().has_value());
    CHECK(never_lost(backend, milliseconds{100}));
    backend.stop();
    CHECK(never_lost(backend, milliseconds{100}));

    const std::uint64_t before = backend.callbacks();
    REQUIRE(backend.start().has_value());
    const bool counting =
        wait_until([&backend, before] { return backend.callbacks() > before; }, milliseconds{1000});
    CHECK(never_lost(backend, milliseconds{100}));
    backend.stop();
    CHECK(counting);
    CHECK(never_lost(backend, milliseconds{100}));
}
