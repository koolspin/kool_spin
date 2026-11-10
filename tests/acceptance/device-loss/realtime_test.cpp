// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/device-loss/spec.md: real-time safety.

#include <ks/core/realtime.h>

#include <ks/engine/block.h>
#include <ks/engine/interleave.h>
#include <ks/engine/render_core.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using ks::engine::Block;
using ks::engine::DriverId;
using ks::engine::frames_to_duration;
using ks::engine::kDefaultDriver;
using ks::engine::kMaxBlockFrames;
using ks::engine::render_interleaved;
using ks::engine::RenderCore;
using ks::engine::RenderResult;
using ks::engine::test::CountingGraph;
using ks::engine::test::Planar;
using ks::engine::test::TimePoint;

constexpr std::uint32_t kRate = 48'000;
constexpr std::size_t kFrames = 64;
constexpr std::size_t kChannels = 2;
constexpr DriverId kDriver{3};

struct Results {
    DriverId active = kDefaultDriver;
    RenderResult skipped = RenderResult::Rejected;
    RenderResult rendered = RenderResult::Rejected;
    TimePoint until;
    std::size_t device_frames = 0;
};

// Every call is checked by function effect analysis at compile time, and by
// RealtimeSanitizer at run time under the rtsan preset.
Results render_as_driver(RenderCore& core, std::span<const std::span<float>> planar,
                         std::span<const std::span<float>> scratch, std::span<float> interleaved,
                         TimePoint host_time) noexcept KS_NONBLOCKING {
    Results results;
    core.activate(kDriver);
    results.active = core.active_driver();
    const Block block{
        .channels = planar, .frames = kFrames, .sample_rate = kRate, .host_time = host_time};
    results.skipped = core.try_render(block, kDefaultDriver);
    results.rendered = core.try_render(block, kDriver);
    results.until = core.rendered_until();
    results.device_frames =
        render_interleaved(core, scratch, interleaved, kChannels, kRate, host_time, kDriver);
    return results;
}

} // namespace

TEST_CASE("Device-loss render path is real-time safe", "[realtime]") {
    CountingGraph graph;
    RenderCore core{graph};
    const Planar planar{kChannels, kFrames, 0.0F};
    const Planar scratch{kChannels, kMaxBlockFrames, 0.0F};
    std::vector<float> interleaved(kChannels * kFrames, 0.0F);
    const TimePoint host_time{std::chrono::seconds{10}};

    const Results results =
        render_as_driver(core, planar.channels(), scratch.channels(), interleaved, host_time);

    REQUIRE(results.active == kDriver);
    REQUIRE(results.skipped == RenderResult::Skipped);
    REQUIRE(results.rendered == RenderResult::Rendered);
    REQUIRE(results.until == host_time + frames_to_duration(kFrames, kRate));
    REQUIRE(results.device_frames == kFrames);
    REQUIRE(core.frames_rendered() == 2 * kFrames);
}
