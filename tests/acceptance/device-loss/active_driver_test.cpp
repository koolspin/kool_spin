// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/device-loss/spec.md: the active driver.

#include <ks/engine/block.h>
#include <ks/engine/render_core.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using ks::engine::Block;
using ks::engine::DriverId;
using ks::engine::frames_to_duration;
using ks::engine::kDefaultDriver;
using ks::engine::RenderCore;
using ks::engine::RenderResult;
using ks::engine::test::BlockRecord;
using ks::engine::test::Clock;
using ks::engine::test::CountingGraph;
using ks::engine::test::JoiningThread;
using ks::engine::test::Planar;
using ks::engine::test::RecordingGraph;
using ks::engine::test::TimePoint;

constexpr std::uint32_t kRate = 48'000;

Block block_of(const Planar& buffers, std::size_t frames, TimePoint host_time = {},
               std::uint32_t sample_rate = kRate) {
    return Block{
        .channels = buffers.channels(),
        .frames = frames,
        .sample_rate = sample_rate,
        .host_time = host_time,
    };
}

} // namespace

TEST_CASE("Active driver: only the active driver renders", "[active-driver]") {
    constexpr DriverId kA{7};
    constexpr DriverId kB{8};
    CountingGraph graph;
    RenderCore core{graph};
    REQUIRE(core.active_driver() == kDefaultDriver);

    Planar buffers{2, 64, 5.0F};
    REQUIRE(core.try_render(block_of(buffers, 64)) == RenderResult::Rendered);
    REQUIRE(buffers.all_equal(1.0F));

    core.activate(kA);
    REQUIRE(core.active_driver() == kA);

    buffers.fill(5.0F);
    REQUIRE(core.try_render(block_of(buffers, 64), kDefaultDriver) == RenderResult::Skipped);
    REQUIRE(buffers.all_equal(0.0F));

    buffers.fill(5.0F);
    REQUIRE(core.try_render(block_of(buffers, 64), kB) == RenderResult::Skipped);
    REQUIRE(buffers.all_equal(0.0F));

    buffers.fill(5.0F);
    REQUIRE(core.try_render(block_of(buffers, 64), kA) == RenderResult::Rendered);
    REQUIRE(buffers.all_equal(1.0F));

    REQUIRE(graph.calls() == 2);
    REQUIRE(core.frames_rendered() == 128);
}

TEST_CASE("Active driver: the first block after a handover is resumed", "[active-driver]") {
    constexpr DriverId kA{1};
    constexpr DriverId kB{2};
    RecordingGraph graph{16};
    RenderCore core{graph};
    const Planar buffers{1, 32, 0.0F};
    const Block block = block_of(buffers, 32);
    Block claims_resumed = block;
    claims_resumed.resumed = true;

    REQUIRE(core.try_render(block) == RenderResult::Rendered); // first ever: resumed
    REQUIRE(core.try_render(block) == RenderResult::Rendered); // same driver: not
    core.activate(kA);
    REQUIRE(core.try_render(block) == RenderResult::Skipped);               // not active
    REQUIRE(core.try_render(block, kA) == RenderResult::Rendered);          // new driver: resumed
    REQUIRE(core.try_render(claims_resumed, kA) == RenderResult::Rendered); // ignored: not
    REQUIRE(core.try_render(block, kA) == RenderResult::Rendered);          // same driver: not
    core.activate(kB);
    REQUIRE(core.try_render(block, kB) == RenderResult::Rendered); // new driver: resumed
    core.activate(kA);
    REQUIRE(core.try_render(block, kA) == RenderResult::Rendered); // back to kA: resumed

    std::vector<bool> resumed;
    for (const BlockRecord& record : graph.records()) {
        resumed.push_back(record.resumed);
    }
    REQUIRE(resumed == std::vector<bool>{true, false, true, false, false, true, true});
}

TEST_CASE("Active driver: rendered_until is where the last block ended", "[active-driver]") {
    using std::chrono::milliseconds;
    using std::chrono::seconds;
    constexpr DriverId kA{3};
    CountingGraph graph;
    RenderCore core{graph};
    REQUIRE(core.rendered_until() == TimePoint{});

    const Planar buffers{2, 480, 0.0F};
    const TimePoint first{seconds{100}};
    REQUIRE(core.try_render(block_of(buffers, 480, first)) == RenderResult::Rendered);
    REQUIRE(core.rendered_until() == first + milliseconds{10});

    // A skipped block doesn't move it.
    core.activate(kA);
    REQUIRE(core.try_render(block_of(buffers, 480, first + seconds{1})) == RenderResult::Skipped);
    REQUIRE(core.rendered_until() == first + milliseconds{10});

    const TimePoint later{seconds{200}};
    REQUIRE(core.try_render(block_of(buffers, 441, later, 44'100), kA) == RenderResult::Rendered);
    REQUIRE(core.rendered_until() == later + frames_to_duration(441, 44'100));
}

TEST_CASE("Active driver: handovers under load", "[active-driver]") {
    constexpr std::size_t kSwitches = 1'000;
    constexpr std::size_t kFrames = 32;
    constexpr std::array<DriverId, 2> kDrivers{DriverId{1}, DriverId{2}};

    CountingGraph graph;
    RenderCore core{graph};
    core.activate(kDrivers[0]);

    std::atomic<bool> done{false};
    std::array<std::string, 2> errors;
    std::array<std::uint64_t, 2> rendered{};
    {
        std::vector<JoiningThread> drivers;
        drivers.reserve(kDrivers.size());
        for (std::size_t d = 0; d < kDrivers.size(); ++d) {
            drivers.emplace_back([&core, &done, &error = errors.at(d), &frames = rendered.at(d),
                                  driver = kDrivers.at(d)] {
                Planar buffers{2, kFrames, 5.0F};
                const Block block = block_of(buffers, kFrames);
                while (!done.load(std::memory_order_relaxed)) {
                    switch (core.try_render(block, driver)) {
                    case RenderResult::Rendered:
                        if (!buffers.all_equal(1.0F)) {
                            error = "a rendered block doesn't hold the graph's output";
                            return;
                        }
                        frames += kFrames;
                        break;
                    case RenderResult::Skipped:
                        if (!buffers.all_equal(0.0F)) {
                            error = "a skipped block isn't silent";
                            return;
                        }
                        std::this_thread::yield();
                        break;
                    case RenderResult::Rejected:
                        error = "a valid block was rejected";
                        return;
                    }
                    buffers.fill(5.0F);
                }
            });
        }

        // Switch back and forth, giving the newly active driver a chance to
        // render each time.
        for (std::size_t n = 1; n <= kSwitches; ++n) {
            const std::uint64_t before = core.frames_rendered();
            core.activate(kDrivers.at(n % 2));
            const TimePoint deadline = Clock::now() + std::chrono::milliseconds{100};
            while (core.frames_rendered() == before && Clock::now() < deadline) {
                std::this_thread::yield();
            }
        }
        done.store(true, std::memory_order_relaxed);
    }

    CAPTURE(errors[0], errors[1]);
    REQUIRE(errors[0].empty());
    REQUIRE(errors[1].empty());
    REQUIRE_FALSE(graph.overlapped());
    REQUIRE(rendered[0] > 0);
    REQUIRE(rendered[1] > 0);
    REQUIRE(core.frames_rendered() == rendered[0] + rendered[1]);
    REQUIRE(graph.frames() == rendered[0] + rendered[1]);
    REQUIRE(graph.resumed() >= 1);
    REQUIRE(graph.resumed() <= kSwitches + 1);
}
