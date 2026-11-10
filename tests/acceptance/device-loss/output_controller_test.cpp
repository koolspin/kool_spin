// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/device-loss/spec.md: the output controller.

#include <ks/engine/output_controller.h>

#include <ks/engine/paced_backend.h>
#include <ks/engine/render_core.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

namespace {

using ks::engine::Loss;
using ks::engine::OutputConfig;
using ks::engine::OutputController;
using ks::engine::OutputState;
using ks::engine::PacedConfig;
using ks::engine::PacedDeviceSource;
using ks::engine::RenderCore;
using ks::engine::SimulatedDevice;
using ks::engine::test::BlockRecord;
using ks::engine::test::Clock;
using ks::engine::test::kTimingTolerance;
using ks::engine::test::RecordingGraph;
using ks::engine::test::seconds_off;
using ks::engine::test::TimePoint;
using ks::engine::test::within_tolerance;

using std::chrono::milliseconds;

constexpr std::uint32_t kRate = 48'000;
constexpr milliseconds kPollInterval{5};

/// Polls every 5 ms for `duration`.
void poll_for(OutputController& controller, milliseconds duration) {
    const TimePoint end = Clock::now() + duration;
    while (Clock::now() < end) {
        controller.poll();
        std::this_thread::sleep_for(kPollInterval);
    }
}

/// Polls every 5 ms until the controller is in `state`, or `timeout` passes.
/// Returns whether it got there.
bool poll_until(OutputController& controller, OutputState state, milliseconds timeout) {
    const TimePoint deadline = Clock::now() + timeout;
    while (controller.state() != state) {
        if (Clock::now() >= deadline) {
            return false;
        }
        controller.poll();
        std::this_thread::sleep_for(kPollInterval);
    }
    return true;
}

/// The largest distance, in seconds, between a block's position (frames
/// rendered before it) and where real time puts it (its host time, from the
/// first block's).
double worst_position_error(std::span<const BlockRecord> records) {
    double worst = 0.0;
    for (const BlockRecord& record : records) {
        const auto elapsed = record.host_time - records.front().host_time;
        worst = std::max(worst, seconds_off(record.first_frame, kRate, elapsed));
    }
    return worst;
}

} // namespace

TEST_CASE("Output: a silent device loss keeps the clock running", "[output]") {
    RecordingGraph graph{100'000};
    RenderCore core{graph};
    SimulatedDevice device;
    PacedDeviceSource source{
        device, PacedConfig{.sample_rate = kRate, .channels = 2, .period_frames = 256}};
    OutputController controller{
        core, source,
        OutputConfig{.stall_timeout = milliseconds{100}, .retry_interval = milliseconds{200}}};

    const TimePoint started = Clock::now();
    controller.start();
    REQUIRE(controller.state() == OutputState::Playing);
    poll_for(controller, milliseconds{300});
    REQUIRE(controller.state() == OutputState::Playing);

    device.unplug(Loss::Silent);
    REQUIRE(poll_until(controller, OutputState::Freewheeling, milliseconds{1000}));
    poll_for(controller, milliseconds{300});
    REQUIRE(controller.state() == OutputState::Freewheeling);

    device.plug_in();
    REQUIRE(poll_until(controller, OutputState::Playing, milliseconds{2000}));
    poll_for(controller, milliseconds{300});
    REQUIRE(controller.state() == OutputState::Playing);

    controller.stop();
    const TimePoint stopped = Clock::now();
    REQUIRE(controller.state() == OutputState::Stopped);

    // The clock kept running.
    const std::uint64_t frames = core.frames_rendered();
    CAPTURE(frames, seconds_off(frames, kRate, stopped - started));
    REQUIRE(within_tolerance(frames, kRate, stopped - started));

    // Every block was where real time puts it.
    const std::span<const BlockRecord> records = graph.records();
    REQUIRE(records.size() == graph.calls());
    REQUIRE_FALSE(records.empty());
    const double worst = worst_position_error(records);
    CAPTURE(worst);
    REQUIRE(worst <= std::chrono::duration<double>(kTimingTolerance).count());

    // Fade-in marks: the first block, the first freewheel block, and the first
    // block from the returned device.
    std::size_t resumed = 0;
    for (const BlockRecord& record : records) {
        resumed += record.resumed ? 1 : 0;
    }
    REQUIRE(records.front().resumed);
    REQUIRE(resumed == 3);

    // Nothing renders after stop().
    const std::size_t calls = graph.calls();
    std::this_thread::sleep_for(milliseconds{100});
    REQUIRE(graph.calls() == calls);
    REQUIRE(core.frames_rendered() == frames);
}

TEST_CASE("Output: a reported loss is noticed at once", "[output]") {
    RecordingGraph graph{0};
    RenderCore core{graph};
    SimulatedDevice device;
    PacedDeviceSource source{
        device, PacedConfig{.sample_rate = kRate, .channels = 2, .period_frames = 256}};
    // A stall timeout this long can't be what notices the loss.
    OutputController controller{core, source,
                                OutputConfig{.stall_timeout = std::chrono::seconds{10},
                                             .retry_interval = milliseconds{200}}};

    controller.start();
    REQUIRE(controller.state() == OutputState::Playing);
    poll_for(controller, milliseconds{100});

    device.unplug(Loss::Reported);
    REQUIRE(poll_until(controller, OutputState::Freewheeling, milliseconds{1000}));
    controller.stop();
    REQUIRE(controller.state() == OutputState::Stopped);
}

TEST_CASE("Output: with no device at start, it freewheels until one appears", "[output]") {
    constexpr std::uint32_t kDeviceRate = 44'100;
    RecordingGraph graph{100'000};
    RenderCore core{graph};
    SimulatedDevice device;
    device.unplug(Loss::Silent);
    PacedDeviceSource source{
        device, PacedConfig{.sample_rate = kDeviceRate, .channels = 2, .period_frames = 256}};
    OutputController controller{
        core, source,
        OutputConfig{.stall_timeout = milliseconds{100}, .retry_interval = milliseconds{200}}};

    const TimePoint started = Clock::now();
    controller.start();
    REQUIRE(controller.state() == OutputState::Freewheeling);
    poll_for(controller, milliseconds{300});
    REQUIRE(controller.state() == OutputState::Freewheeling);

    // Freewheeling at the default format, keeping up with real time.
    const std::uint64_t frames = core.frames_rendered();
    const TimePoint measured = Clock::now();
    CAPTURE(frames, seconds_off(frames, kRate, measured - started));
    REQUIRE(within_tolerance(frames, kRate, measured - started));
    REQUIRE_FALSE(graph.records().empty());
    for (const BlockRecord& record : graph.records()) {
        REQUIRE(record.sample_rate == kRate);
        REQUIRE(record.channels == 2);
    }

    device.plug_in();
    REQUIRE(poll_until(controller, OutputState::Playing, milliseconds{2000}));
    poll_for(controller, milliseconds{50});
    controller.stop();

    // The device's own format took over.
    REQUIRE(graph.records().back().sample_rate == kDeviceRate);
}
