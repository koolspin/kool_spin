// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/device-loss/spec.md: the paced backend and
// simulated devices.

#include <ks/engine/paced_backend.h>

#include <ks/engine/audio_backend.h>
#include <ks/engine/block.h>
#include <ks/engine/render_core.h>
#include <ks/engine/safety.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <thread>

namespace {

using ks::engine::BackendError;
using ks::engine::frames_to_duration;
using ks::engine::kMaxBlockFrames;
using ks::engine::kMaxOutputChannels;
using ks::engine::Loss;
using ks::engine::open_paced;
using ks::engine::PacedBackend;
using ks::engine::PacedConfig;
using ks::engine::Priority;
using ks::engine::RenderCore;
using ks::engine::SimulatedDevice;
using ks::engine::test::BlockRecord;
using ks::engine::test::Clock;
using ks::engine::test::CountingGraph;
using ks::engine::test::RecordingGraph;
using ks::engine::test::seconds_off;
using ks::engine::test::signal;
using ks::engine::test::TimePoint;
using ks::engine::test::wait_until;
using ks::engine::test::within_tolerance;

using Opened = std::expected<std::unique_ptr<PacedBackend>, BackendError>;
using std::chrono::milliseconds;

constexpr std::uint32_t kRate = 48'000;

std::string error_text(const Opened& opened) {
    return opened.has_value() ? std::string{} : opened.error().message;
}

/// Waits up to a second for the core's frame count to pass `frames`.
bool wait_for_frames_past(const RenderCore& core, std::uint64_t frames) {
    return wait_until([&core, frames] { return core.frames_rendered() > frames; },
                      milliseconds{1000});
}

/// True if the core's frame count doesn't move for `period`.
bool stays_still(const RenderCore& core, milliseconds period) {
    const std::uint64_t before = core.frames_rendered();
    std::this_thread::sleep_for(period);
    return core.frames_rendered() == before;
}

/// Passes in-range samples through unchanged, so the samples can be checked
/// exactly. The safety stage still runs (specs/safety-stage/spec.md, req. 18).
const ks::engine::SafetyConfig kTransparent{.fade = std::chrono::milliseconds{0},
                                            .ceiling_dbfs = 0.0};

} // namespace

TEST_CASE("Paced: renders in real time", "[paced]") {
    constexpr std::size_t kPeriod = 1024;
    RecordingGraph graph{100'000};
    RenderCore core{graph};
    const Opened opened = open_paced(core, PacedConfig{
                                               .sample_rate = kRate,
                                               .channels = 2,
                                               .period_frames = kPeriod,
                                           });
    CAPTURE(error_text(opened));
    REQUIRE(opened.has_value());
    PacedBackend& backend = **opened;
    REQUIRE(backend.info().backend == "Paced");
    REQUIRE(backend.info().sample_rate == kRate);
    REQUIRE(backend.info().channels == 2);
    REQUIRE(backend.info().period_frames == kPeriod);

    const TimePoint before_start = Clock::now();
    REQUIRE(backend.start().has_value());
    const TimePoint after_start = Clock::now();
    std::this_thread::sleep_for(milliseconds{500});
    backend.stop();
    const TimePoint stopped = Clock::now();

    const std::uint64_t frames = core.frames_rendered();
    CAPTURE(frames, seconds_off(frames, kRate, stopped - before_start));
    REQUIRE(within_tolerance(frames, kRate, stopped - before_start));
    REQUIRE(backend.callbacks() * kPeriod == frames);

    const std::span<const BlockRecord> records = graph.records();
    REQUIRE_FALSE(records.empty());
    const TimePoint first = records.front().host_time;
    REQUIRE(first >= before_start);
    REQUIRE(first <= after_start);
    std::uint64_t offset = 0;
    for (const BlockRecord& record : records) {
        CAPTURE(offset);
        REQUIRE(record.frames <= kMaxBlockFrames);
        // Period n starts at frames_to_duration(n × period); each block in it
        // at the period's time + frames_to_duration(frames before it).
        const std::uint64_t whole_periods = offset / kPeriod * kPeriod;
        const std::uint64_t within_period = offset % kPeriod;
        REQUIRE(record.host_time == first + frames_to_duration(whole_periods, kRate) +
                                        frames_to_duration(within_period, kRate));
        offset += record.frames;
    }
}

TEST_CASE("Paced: records what it plays", "[paced]") {
    constexpr std::size_t kRecorded = 4800;
    constexpr std::size_t kChannels = 2;
    RecordingGraph graph{0};
    RenderCore core{graph, kTransparent};
    const Opened opened = open_paced(core, PacedConfig{
                                               .sample_rate = kRate,
                                               .channels = kChannels,
                                               .period_frames = 256,
                                               .record_frames = kRecorded,
                                           });
    CAPTURE(error_text(opened));
    REQUIRE(opened.has_value());
    PacedBackend& backend = **opened;

    REQUIRE(backend.start().has_value());
    REQUIRE(
        wait_until([&core] { return core.frames_rendered() > 2 * kRecorded; }, milliseconds{2000}));
    backend.stop();

    const std::span<const float> recording = backend.recording();
    REQUIRE(recording.size() == kRecorded * kChannels);
    std::string mismatch;
    for (std::size_t f = 0; f < kRecorded && mismatch.empty(); ++f) {
        for (std::size_t c = 0; c < kChannels; ++c) {
            if (recording[(f * kChannels) + c] != signal(f, c)) {
                mismatch = "frame " + std::to_string(f) + ", channel " + std::to_string(c);
                break;
            }
        }
    }
    CAPTURE(mismatch);
    REQUIRE(mismatch.empty());
}

TEST_CASE("Paced: catches up from a start time in the past", "[paced]") {
    RecordingGraph graph{100'000};
    RenderCore core{graph};
    const TimePoint start_time = Clock::now() - milliseconds{200};
    const Opened opened = open_paced(core, PacedConfig{
                                               .sample_rate = kRate,
                                               .channels = 2,
                                               .period_frames = 256,
                                               .start_time = start_time,
                                           });
    CAPTURE(error_text(opened));
    REQUIRE(opened.has_value());
    PacedBackend& backend = **opened;

    REQUIRE(backend.start().has_value());
    const bool caught_up = wait_until(
        [&core, start_time] {
            return within_tolerance(core.frames_rendered(), kRate, Clock::now() - start_time);
        },
        milliseconds{100});
    backend.stop();

    REQUIRE(caught_up);
    REQUIRE_FALSE(graph.records().empty());
    REQUIRE(graph.records().front().host_time == start_time);
}

TEST_CASE("Paced: stop, start, and invalid configurations", "[paced]") {
    CountingGraph graph;
    RenderCore core{graph};
    const Opened opened =
        open_paced(core, PacedConfig{.sample_rate = kRate, .channels = 2, .period_frames = 256});
    CAPTURE(error_text(opened));
    REQUIRE(opened.has_value());
    PacedBackend& backend = **opened;

    REQUIRE(backend.start().has_value());
    REQUIRE(wait_for_frames_past(core, 0));
    backend.stop();
    REQUIRE(stays_still(core, milliseconds{100}));

    const std::uint64_t stopped_at = core.frames_rendered();
    REQUIRE(backend.start().has_value());
    REQUIRE(wait_for_frames_past(core, stopped_at));
    backend.stop();

    struct Case {
        std::string name;
        PacedConfig config;
        BackendError::Kind kind;
    };
    const std::array cases{
        Case{.name = "no channels",
             .config = {.sample_rate = kRate, .channels = 0, .period_frames = 256},
             .kind = BackendError::Kind::OpenFailed},
        Case{.name = "more than kMaxOutputChannels",
             .config = {.sample_rate = kRate,
                        .channels = static_cast<std::uint32_t>(kMaxOutputChannels + 1),
                        .period_frames = 256},
             .kind = BackendError::Kind::TooManyChannels},
        Case{.name = "a sample rate of 0",
             .config = {.sample_rate = 0, .channels = 2, .period_frames = 256},
             .kind = BackendError::Kind::OpenFailed},
        Case{.name = "a period of 0",
             .config = {.sample_rate = kRate, .channels = 2, .period_frames = 0},
             .kind = BackendError::Kind::OpenFailed},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        const Opened refused = open_paced(core, c.config);
        REQUIRE_FALSE(refused.has_value());
        REQUIRE(refused.error().kind == c.kind);
    }
}

TEST_CASE("Paced: real-time priority is reported", "[paced]") {
    for (const bool requested : {true, false}) {
        CAPTURE(requested);
        CountingGraph graph;
        RenderCore core{graph};
        const Opened opened = open_paced(core, PacedConfig{
                                                   .sample_rate = kRate,
                                                   .channels = 2,
                                                   .period_frames = 256,
                                                   .realtime_priority = requested,
                                               });
        CAPTURE(error_text(opened));
        REQUIRE(opened.has_value());
        PacedBackend& backend = **opened;

        REQUIRE(backend.start().has_value());
        REQUIRE(wait_until([&backend] { return backend.callbacks() > 0; }, milliseconds{1000}));
        const Priority priority = backend.priority();
        backend.stop();

        if (requested) {
            REQUIRE((priority == Priority::Granted || priority == Priority::Refused));
        } else {
            REQUIRE(priority == Priority::NotRequested);
        }
    }
}

TEST_CASE("Simulated device: unplugging stops the backends on it", "[paced]") {
    for (const Loss loss : {Loss::Silent, Loss::Reported}) {
        CAPTURE(loss == Loss::Reported);
        CountingGraph graph;
        RenderCore core{graph};
        SimulatedDevice device;
        REQUIRE(device.present());
        const PacedConfig config{
            .sample_rate = kRate, .channels = 2, .period_frames = 256, .device = &device};

        const Opened first = open_paced(core, config);
        CAPTURE(error_text(first));
        REQUIRE(first.has_value());
        REQUIRE((*first)->start().has_value());
        REQUIRE(wait_for_frames_past(core, 0));
        REQUIRE_FALSE((*first)->lost());

        device.unplug(loss);
        REQUIRE_FALSE(device.present());
        // Stopped within 50 ms: still from 50 ms on.
        std::this_thread::sleep_for(milliseconds{50});
        const std::uint64_t callbacks = (*first)->callbacks();
        REQUIRE(stays_still(core, milliseconds{50}));
        REQUIRE((*first)->callbacks() == callbacks);
        REQUIRE((*first)->lost() == (loss == Loss::Reported));

        // Plugging back in doesn't restart it...
        device.plug_in();
        REQUIRE(device.present());
        REQUIRE(stays_still(core, milliseconds{100}));

        // ...but a backend opened on it now renders.
        const std::uint64_t frames = core.frames_rendered();
        const Opened second = open_paced(core, config);
        CAPTURE(error_text(second));
        REQUIRE(second.has_value());
        REQUIRE((*second)->start().has_value());
        REQUIRE(wait_for_frames_past(core, frames));
        REQUIRE_FALSE((*second)->lost());

        (*second)->stop();
        (*first)->stop();
    }
}
