// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/render-core/spec.md: the render core.

#include <ks/engine/render_core.h>

#include <ks/engine/block.h>
#include <ks/engine/safety.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using ks::engine::Block;
using ks::engine::frames_to_duration;
using ks::engine::kMaxBlockFrames;
using ks::engine::kMaxOutputChannels;
using ks::engine::RenderCore;
using ks::engine::RenderResult;
using ks::engine::test::ConstantGraph;
using ks::engine::test::JoiningThread;
using ks::engine::test::Planar;
using ks::engine::test::ReentrantGraph;
using ks::engine::test::SilentGraph;

constexpr std::uint32_t kRate = 48'000;

Block block_of(const Planar& buffers, std::size_t frames, std::uint32_t sample_rate = kRate) {
    return Block{
        .channels = buffers.channels(),
        .frames = frames,
        .sample_rate = sample_rate,
        .host_time = {},
    };
}

/// Passes in-range samples through unchanged, so the samples can be checked
/// exactly. The safety stage still runs (specs/safety-stage/spec.md, req. 18).
const ks::engine::SafetyConfig kTransparent{.fade = std::chrono::milliseconds{0},
                                            .ceiling_dbfs = 0.0};

} // namespace

TEST_CASE("Render core: channels are cleared, then rendered", "[render-core]") {
    SilentGraph graph;
    RenderCore core{graph};
    REQUIRE(core.frames_rendered() == 0);

    const Planar buffers{2, 64, 5.0F};
    REQUIRE(core.try_render(block_of(buffers, 64)) == RenderResult::Rendered);
    REQUIRE(graph.calls() == 1);
    REQUIRE(buffers.all_equal(0.0F));
    REQUIRE(core.frames_rendered() == 64);

    const Planar more{2, 32, 5.0F};
    REQUIRE(core.try_render(block_of(more, 32)) == RenderResult::Rendered);
    REQUIRE(graph.calls() == 2);
    REQUIRE(more.all_equal(0.0F));
    REQUIRE(core.frames_rendered() == 96);
}

TEST_CASE("Render core: invalid blocks are rejected", "[render-core]") {
    struct Case {
        std::string name;
        std::vector<std::size_t> lengths; // one per channel
        std::size_t frames;
        std::uint32_t sample_rate;
    };
    const std::vector<Case> cases{
        {.name = "no frames", .lengths = {64, 64}, .frames = 0, .sample_rate = kRate},
        {.name = "more than kMaxBlockFrames",
         .lengths = {kMaxBlockFrames + 1, kMaxBlockFrames + 1},
         .frames = kMaxBlockFrames + 1,
         .sample_rate = kRate},
        {.name = "no channels", .lengths = {}, .frames = 64, .sample_rate = kRate},
        {.name = "more than kMaxOutputChannels",
         .lengths = std::vector<std::size_t>(kMaxOutputChannels + 1, 64),
         .frames = 64,
         .sample_rate = kRate},
        {.name = "a channel shorter than the frame count",
         .lengths = {64, 63},
         .frames = 64,
         .sample_rate = kRate},
        {.name = "a sample rate of 0", .lengths = {64, 64}, .frames = 64, .sample_rate = 0},
    };

    for (const Case& c : cases) {
        CAPTURE(c.name);
        SilentGraph graph;
        RenderCore core{graph};
        const Planar buffers{c.lengths, 5.0F};
        REQUIRE(core.try_render(block_of(buffers, c.frames, c.sample_rate)) ==
                RenderResult::Rejected);
        REQUIRE(buffers.all_equal(5.0F));
        REQUIRE(graph.calls() == 0);
        REQUIRE(core.frames_rendered() == 0);
    }

    // The limits themselves are valid, and channels may be longer than the block.
    SilentGraph graph;
    RenderCore core{graph};
    const Planar widest{kMaxOutputChannels, kMaxBlockFrames, 5.0F};
    REQUIRE(core.try_render(block_of(widest, kMaxBlockFrames)) == RenderResult::Rendered);
    const Planar longer{{100, 64}, 5.0F};
    REQUIRE(core.try_render(block_of(longer, 64)) == RenderResult::Rendered);
    const Planar one{1, 1, 5.0F};
    REQUIRE(core.try_render(block_of(one, 1, 1)) == RenderResult::Rendered);
    REQUIRE(graph.calls() == 3);
    REQUIRE(core.frames_rendered() == kMaxBlockFrames + 64 + 1);
}

TEST_CASE("Render core: a block that arrives mid-render is skipped", "[render-core]") {
    ReentrantGraph graph;
    RenderCore core{graph};
    const Planar inner{2, 32, 7.0F};
    graph.set_inner(core, block_of(inner, 32));

    const Planar outer{2, 64, 5.0F};
    REQUIRE(core.try_render(block_of(outer, 64)) == RenderResult::Rendered);
    REQUIRE(graph.inner_result() == RenderResult::Skipped);
    REQUIRE(inner.all_equal(0.0F));
    REQUIRE(core.frames_rendered() == 64);
}

TEST_CASE("Render core: only one driver renders at a time", "[render-core]") {
    constexpr std::size_t kBlocks = 100'000;
    constexpr std::size_t kFrames = 32;
    constexpr float kRendered = 1.0F;

    ConstantGraph graph{kRendered};
    RenderCore core{graph, kTransparent};

    struct Report {
        std::uint64_t rendered_frames = 0;
        std::string error;
    };
    std::array<Report, 2> reports{};
    {
        std::vector<JoiningThread> drivers;
        drivers.reserve(reports.size());
        for (Report& report : reports) {
            drivers.emplace_back([&core, &report] {
                Planar buffers{2, kFrames, 5.0F};
                const Block block = block_of(buffers, kFrames);
                for (std::size_t n = 0; n < kBlocks; ++n) {
                    switch (core.try_render(block)) {
                    case RenderResult::Rendered:
                        if (!buffers.all_equal(kRendered)) {
                            report.error = "a rendered block doesn't hold the graph's output";
                            return;
                        }
                        report.rendered_frames += kFrames;
                        break;
                    case RenderResult::Skipped:
                        if (!buffers.all_equal(0.0F)) {
                            report.error = "a skipped block isn't silent";
                            return;
                        }
                        break;
                    case RenderResult::Rejected:
                        report.error = "a valid block was rejected";
                        return;
                    }
                    buffers.fill(5.0F);
                }
            });
        }
    }

    CAPTURE(reports[0].error, reports[1].error);
    REQUIRE(reports[0].error.empty());
    REQUIRE(reports[1].error.empty());
    REQUIRE_FALSE(graph.overlapped());
    const std::uint64_t rendered = reports[0].rendered_frames + reports[1].rendered_frames;
    REQUIRE(rendered > 0);
    REQUIRE(core.frames_rendered() == rendered);
    REQUIRE(graph.frames() == rendered);
}

TEST_CASE("Render core: frames_to_duration is exact", "[render-core]") {
    using std::chrono::nanoseconds;
    constexpr std::uint64_t kHuge = std::uint64_t{1} << 40U;

    // For each rate: floor(frames × 10⁹ / rate) for 0 frames, 1 frame, one
    // frame short of a second, one second, and 2⁴⁰ frames, worked out exactly.
    struct Rate {
        std::uint32_t sample_rate;
        std::array<std::int64_t, 5> expected_ns;
    };
    const std::array<Rate, 4> rates{
        Rate{.sample_rate = 44'100,
             .expected_ns = {0, 22'675, 999'977'324, 1'000'000'000, 24'932'236'457'505'668}},
        Rate{.sample_rate = 48'000,
             .expected_ns = {0, 20'833, 999'979'166, 1'000'000'000, 22'906'492'245'333'333}},
        Rate{.sample_rate = 96'000,
             .expected_ns = {0, 10'416, 999'989'583, 1'000'000'000, 11'453'246'122'666'666}},
        Rate{.sample_rate = 192'000,
             .expected_ns = {0, 5'208, 999'994'791, 1'000'000'000, 5'726'623'061'333'333}},
    };
    for (const Rate& rate : rates) {
        const std::array<std::uint64_t, 5> frames{0, 1, rate.sample_rate - 1, rate.sample_rate,
                                                  kHuge};
        for (std::size_t i = 0; i < frames.size(); ++i) {
            CAPTURE(rate.sample_rate, frames.at(i));
            REQUIRE(frames_to_duration(frames.at(i), rate.sample_rate) ==
                    nanoseconds{rate.expected_ns.at(i)});
        }
    }

    // A sample rate of 0 gives 0.
    for (const std::uint64_t frames : {std::uint64_t{0}, std::uint64_t{1}, kHuge}) {
        CAPTURE(frames);
        REQUIRE(frames_to_duration(frames, 0) == nanoseconds{0});
    }
}
