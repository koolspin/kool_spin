// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/render-core/spec.md: real-time safety.

#include <ks/core/realtime.h>

#include <ks/core/clock.h>
#include <ks/engine/block.h>
#include <ks/engine/interleave.h>
#include <ks/engine/offline.h>
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
using ks::engine::frames_to_duration;
using ks::engine::interleave;
using ks::engine::kMaxBlockFrames;
using ks::engine::OfflineConfig;
using ks::engine::render_interleaved;
using ks::engine::render_offline;
using ks::engine::RenderCore;
using ks::engine::RenderResult;
using ks::engine::test::Planar;
using ks::engine::test::SignalGraph;

constexpr std::uint32_t kRate = 48'000;
constexpr std::size_t kFrames = 64;
constexpr std::size_t kChannels = 2;

struct Results {
    std::chrono::steady_clock::time_point now;
    std::chrono::nanoseconds one_second{};
    RenderResult rendered = RenderResult::Rejected;
    std::uint64_t frames_after_render = 0;
    std::size_t offline_frames = 0;
    bool interleaved = false;
    std::size_t device_frames = 0;
};

// Every call is checked by function effect analysis at compile time, and by
// RealtimeSanitizer at run time under the rtsan preset.
Results render_everything(RenderCore& core, std::span<const std::span<float>> planar,
                          std::span<const std::span<float>> scratch,
                          std::span<float> interleaved) noexcept KS_NONBLOCKING {
    Results results;
    results.now = ks::core::monotonic_now();
    results.one_second = frames_to_duration(kRate, kRate);

    const Block block{
        .channels = planar, .frames = kFrames, .sample_rate = kRate, .host_time = results.now};
    results.rendered = core.try_render(block);
    results.frames_after_render = core.frames_rendered();

    const OfflineConfig config{
        .sample_rate = kRate, .block_frames = kFrames, .start_time = results.now};
    results.offline_frames = render_offline(core, config, planar);
    results.interleaved = interleave(planar, kFrames, interleaved);
    results.device_frames =
        render_interleaved(core, scratch, interleaved, kChannels, kRate, results.now);
    return results;
}

} // namespace

TEST_CASE("Render path is real-time safe", "[realtime]") {
    SignalGraph graph{0};
    RenderCore core{graph};
    const Planar planar{kChannels, kFrames, 0.0F};
    const Planar scratch{kChannels, kMaxBlockFrames, 0.0F};
    std::vector<float> interleaved(kChannels * kFrames, 0.0F);

    const auto before = std::chrono::steady_clock::now();
    const Results results =
        render_everything(core, planar.channels(), scratch.channels(), interleaved);
    const auto after = std::chrono::steady_clock::now();

    // monotonic_now() reads the steady clock.
    REQUIRE(results.now >= before);
    REQUIRE(results.now <= after);
    REQUIRE(results.one_second == std::chrono::seconds{1});
    REQUIRE(results.rendered == RenderResult::Rendered);
    REQUIRE(results.frames_after_render == kFrames);
    REQUIRE(results.offline_frames == kFrames);
    REQUIRE(results.interleaved);
    REQUIRE(results.device_frames == kFrames);
    REQUIRE(core.frames_rendered() == 3 * kFrames);
}
