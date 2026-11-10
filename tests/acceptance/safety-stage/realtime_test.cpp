// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/safety-stage/spec.md: real-time safety.

#include <ks/core/realtime.h>

#include <ks/engine/block.h>
#include <ks/engine/render_core.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using ks::engine::Block;
using ks::engine::RenderCore;
using ks::engine::RenderResult;
using ks::engine::test::FunctionGraph;
using ks::engine::test::kRate;

constexpr std::size_t kFrames = 64;

struct Results {
    RenderResult rendered = RenderResult::Rejected;
    bool silenced = true;
    std::uint64_t non_finite = 1;
    std::uint64_t limited = 1;
};

// Every call, and the safety stage inside try_render(), is checked by function
// effect analysis at compile time, and by RealtimeSanitizer at run time under
// the rtsan preset.
Results render_with_safety(RenderCore& core,
                           std::span<const std::span<float>> channels) noexcept KS_NONBLOCKING {
    Results results;
    core.set_muted(false);
    const Block block{
        .channels = channels, .frames = kFrames, .sample_rate = kRate, .host_time = {}};
    results.rendered = core.try_render(block);
    results.silenced = core.silenced();
    results.non_finite = core.non_finite_samples();
    results.limited = core.limited_frames();
    return results;
}

} // namespace

TEST_CASE("Safety stage is real-time safe", "[realtime]") {
    FunctionGraph graph{[](std::uint64_t /*frame*/, std::size_t /*channel*/) { return 0.25F; }};
    RenderCore core{graph};
    std::array<std::vector<float>, 2> storage{std::vector<float>(kFrames),
                                              std::vector<float>(kFrames)};
    const std::array<std::span<float>, 2> channels{std::span{storage[0]}, std::span{storage[1]}};

    const Results results = render_with_safety(core, channels);
    REQUIRE(results.rendered == RenderResult::Rendered);
    REQUIRE_FALSE(results.silenced);
    REQUIRE(results.non_finite == 0);
    REQUIRE(results.limited == 0);
    // The first block fades in: frame 0 is silent, the last isn't.
    REQUIRE(storage[0].front() == 0.0F);
    REQUIRE(storage[0].back() > 0.0F);
    REQUIRE(storage[0].back() < 0.25F);
}
