// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/miniaudio-devices/spec.md: the device source,
// and the output controller playing through it.

#include <ks/engine/audio_backend.h>
#include <ks/engine/miniaudio_backend.h>
#include <ks/engine/output_controller.h>
#include <ks/engine/render_core.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <thread>

namespace {

using ks::engine::AudioBackend;
using ks::engine::BackendError;
using ks::engine::DriverId;
using ks::engine::list_output_devices;
using ks::engine::MiniaudioBackends;
using ks::engine::MiniaudioDeviceSource;
using ks::engine::MiniaudioSourceConfig;
using ks::engine::OutputConfig;
using ks::engine::OutputController;
using ks::engine::OutputDevice;
using ks::engine::OutputDeviceList;
using ks::engine::OutputState;
using ks::engine::RenderCore;
using ks::engine::StreamInfo;
using ks::engine::test::Clock;
using ks::engine::test::error_text;
using ks::engine::test::flipped;
using ks::engine::test::seconds_off;
using ks::engine::test::SilentGraph;
using ks::engine::test::TimePoint;
using ks::engine::test::wait_until;
using ks::engine::test::within_tolerance;

using std::chrono::milliseconds;
using Created = std::expected<std::unique_ptr<MiniaudioDeviceSource>, BackendError>;
using Opened = std::expected<std::unique_ptr<AudioBackend>, BackendError>;

constexpr std::uint32_t kRate = 48'000;
constexpr std::uint32_t kPeriod = 256;
constexpr DriverId kDriver{7};
constexpr milliseconds kPollInterval{5};

/// The null backend's one device.
OutputDevice null_device() {
    const std::expected<OutputDeviceList, BackendError> listed =
        list_output_devices(MiniaudioBackends::Null);
    CAPTURE(error_text(listed));
    REQUIRE(listed.has_value());
    REQUIRE(listed->devices.size() == 1);
    return listed->devices.front();
}

/// Polls every 5 ms for `duration`. Returns whether the controller stayed in
/// `state` throughout.
bool poll_in_state(OutputController& controller, OutputState state, milliseconds duration) {
    bool stayed = controller.state() == state;
    const TimePoint end = Clock::now() + duration;
    while (Clock::now() < end) {
        controller.poll();
        stayed = stayed && controller.state() == state;
        std::this_thread::sleep_for(kPollInterval);
    }
    return stayed;
}

} // namespace

TEST_CASE("Miniaudio source: resolving and opening the device", "[miniaudio-devices]") {
    const OutputDevice device = null_device();

    SECTION("with no key, it resolves the default device") {
        const Created created = MiniaudioDeviceSource::create(
            MiniaudioSourceConfig{.backends = MiniaudioBackends::Null});
        CAPTURE(error_text(created));
        REQUIRE(created.has_value());
        CHECK((*created)->device().key == device.key);
        CHECK((*created)->device().name == device.name);
    }

    SECTION("with the device's key, it resolves the same device, with its name") {
        const Created created = MiniaudioDeviceSource::create(
            MiniaudioSourceConfig{.backends = MiniaudioBackends::Null, .device = device.key});
        CAPTURE(error_text(created));
        REQUIRE(created.has_value());
        CHECK((*created)->device().key == device.key);
        CHECK((*created)->device().name == device.name);
    }

    SECTION("with a key no device has, the device isn't available") {
        const auto missing = flipped(device.key, 0);
        const Created created = MiniaudioDeviceSource::create(
            MiniaudioSourceConfig{.backends = MiniaudioBackends::Null, .device = missing});
        CAPTURE(error_text(created));
        REQUIRE(created.has_value());
        MiniaudioDeviceSource& source = **created;
        CHECK(source.device().key == missing);
        CHECK(source.device().name.empty());
        CHECK_FALSE(source.available());

        SilentGraph graph;
        RenderCore core{graph};
        const Opened opened = source.open(core, kDriver);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().kind == BackendError::Kind::OpenFailed);
    }

    SECTION("it opens the device, not started, rendering as the given driver") {
        const Created created = MiniaudioDeviceSource::create(MiniaudioSourceConfig{
            .backends = MiniaudioBackends::Null,
            .channels = 2,
            .sample_rate = kRate,
            .period_frames = kPeriod,
        });
        CAPTURE(error_text(created));
        REQUIRE(created.has_value());
        MiniaudioDeviceSource& source = **created;
        CHECK(source.available());

        SilentGraph graph;
        RenderCore core{graph};
        Opened opened = source.open(core, kDriver);
        CAPTURE(error_text(opened));
        REQUIRE(opened.has_value());
        AudioBackend& backend = **opened;
        CHECK(backend.info().period_frames == kPeriod);

        // Not started: it doesn't call back.
        std::this_thread::sleep_for(milliseconds{50});
        CHECK(backend.callbacks() == 0);

        // Started while another driver is active, its blocks are skipped.
        REQUIRE(backend.start().has_value());
        REQUIRE(wait_until([&backend] { return backend.callbacks() > 1; }, milliseconds{1000}));
        CHECK(core.frames_rendered() == 0);

        // Once its driver is active, they render.
        core.activate(kDriver);
        const bool rendered =
            wait_until([&core] { return core.frames_rendered() > 0; }, milliseconds{1000});
        backend.stop();
        REQUIRE(rendered);

        // A second open has the first one's format.
        const StreamInfo first = backend.info();
        opened->reset();
        const Opened again = source.open(core, DriverId{8});
        CAPTURE(error_text(again));
        REQUIRE(again.has_value());
        CHECK((*again)->info().sample_rate == first.sample_rate);
        CHECK((*again)->info().channels == first.channels);
    }
}

TEST_CASE("Output: playing through a miniaudio device", "[miniaudio-devices]") {
    SilentGraph graph;
    RenderCore core{graph};
    const Created created = MiniaudioDeviceSource::create(MiniaudioSourceConfig{
        .backends = MiniaudioBackends::Null,
        .channels = 2,
        .sample_rate = kRate,
        .period_frames = kPeriod,
    });
    CAPTURE(error_text(created));
    REQUIRE(created.has_value());
    OutputController controller{core, **created, OutputConfig{}};

    const TimePoint started = Clock::now();
    controller.start();
    REQUIRE(controller.state() == OutputState::Playing);
    CHECK(poll_in_state(controller, OutputState::Playing, milliseconds{500}));

    // A busy test runner can wake the device's thread late, so it gets up to
    // 100 ms to catch up. One that runs slow or fast never does.
    const bool in_time = wait_until(
        [&core, &controller, started] {
            controller.poll();
            return within_tolerance(core.frames_rendered(), kRate, Clock::now() - started);
        },
        milliseconds{100});
    const std::uint64_t frames_checked = core.frames_rendered();
    const TimePoint checked = Clock::now();
    CAPTURE(frames_checked, seconds_off(frames_checked, kRate, checked - started));
    CHECK(in_time);

    controller.stop();
    REQUIRE(controller.state() == OutputState::Stopped);
    const std::uint64_t frames = core.frames_rendered();
    std::this_thread::sleep_for(milliseconds{100});
    CHECK(core.frames_rendered() == frames);
}
