// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/miniaudio-devices/spec.md: which device events
// mean the device is lost.

#include <ks/engine/miniaudio_backend.h>

#include <catch2/catch_test_macros.hpp>

#include <array>

namespace {

using ks::engine::DeviceEvent;
using ks::engine::LossDetector;

/// The events that lose the device even while a stop is in progress.
constexpr std::array kAlwaysLost{DeviceEvent::Rerouted, DeviceEvent::InterruptionBegan};

/// The events that never lose the device.
constexpr std::array kNeverLost{DeviceEvent::Started, DeviceEvent::InterruptionEnded,
                                DeviceEvent::Unlocked};

} // namespace

TEST_CASE("Loss detector: which events mean the device is lost", "[miniaudio-devices]") {
    SECTION("a new detector isn't lost") {
        const LossDetector detector;
        CHECK_FALSE(detector.lost());
    }

    SECTION("Stopped makes it lost") {
        LossDetector detector;
        detector.on_event(DeviceEvent::Stopped);
        CHECK(detector.lost());
    }

    SECTION("Stopped while a stop is in progress doesn't") {
        LossDetector detector;
        detector.stopping();
        detector.on_event(DeviceEvent::Stopped);
        CHECK_FALSE(detector.lost());
    }

    SECTION("Stopped after starting() makes it lost again") {
        LossDetector detector;
        detector.stopping();
        detector.on_event(DeviceEvent::Stopped);
        detector.starting();
        detector.on_event(DeviceEvent::Started);
        CHECK_FALSE(detector.lost());
        detector.on_event(DeviceEvent::Stopped);
        CHECK(detector.lost());
    }

    SECTION("Rerouted and InterruptionBegan make it lost, even during a stop") {
        for (const DeviceEvent event : kAlwaysLost) {
            CAPTURE(event);
            LossDetector running;
            running.on_event(event);
            CHECK(running.lost());

            LossDetector stopping;
            stopping.stopping();
            stopping.on_event(event);
            CHECK(stopping.lost());
        }
    }

    SECTION("Started, InterruptionEnded, and Unlocked don't") {
        for (const DeviceEvent event : kNeverLost) {
            CAPTURE(event);
            LossDetector running;
            running.on_event(event);
            CHECK_FALSE(running.lost());

            LossDetector stopping;
            stopping.stopping();
            stopping.on_event(event);
            CHECK_FALSE(stopping.lost());
        }
    }

    SECTION("once lost, it stays lost") {
        LossDetector detector;
        detector.on_event(DeviceEvent::Stopped);
        REQUIRE(detector.lost());
        detector.starting();
        for (const DeviceEvent event : kNeverLost) {
            detector.on_event(event);
            CHECK(detector.lost());
        }
        detector.stopping();
        detector.on_event(DeviceEvent::Stopped);
        CHECK(detector.lost());
        detector.starting();
        CHECK(detector.lost());
    }
}
