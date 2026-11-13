// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/resume-state/spec.md: real-time safety
// (requirement 18).

#include <ks/core/realtime.h>

#include <ks/engine/block.h>
#include <ks/engine/engine.h>
#include <ks/engine/render_core.h>
#include <ks/engine/track.h>
#include <ks/ipc/state.h>

#include "../commands/test_support.h"
#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace {

using namespace std::chrono_literals;
using ks::engine::Block;
using ks::engine::RenderCore;
using ks::engine::RenderResult;
using ks::engine::resume_test::Clock;
using ks::engine::resume_test::kBlock;
using ks::engine::resume_test::kRate;

// Every call here, and everything try_render() does inside, the snapshot
// writes, the first block's moves, and the fade included, is checked by
// function effect analysis at compile time, and by RealtimeSanitizer at run
// time under the rtsan preset.
std::uint64_t render_blocks(RenderCore& core, std::span<const std::span<float>> channels,
                            Clock::time_point start, std::size_t count) noexcept KS_NONBLOCKING {
    std::uint64_t rendered = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const Block block{.channels = channels,
                          .frames = kBlock,
                          .sample_rate = kRate,
                          .host_time = start + ks::engine::frames_to_duration(i * kBlock, kRate),
                          .output_latency = {},
                          .resumed = false};
        rendered += core.try_render(block) == RenderResult::Rendered ? 1 : 0;
    }
    return rendered;
}

} // namespace

TEST_CASE("Resume is real-time safe", "[resume][realtime]") {
    const auto track = ks::engine::resume_test::music(601);
    const std::array<const ks::engine::Track*, 1> tracks{track->get()};
    const ks::engine::state_test::Channels channels;

    // Engine A plays a track and writes snapshots.
    auto state = std::make_unique<ks::ipc::EngineState>();
    auto a = std::make_unique<ks::engine::Engine>(*state);
    RenderCore core_a{*a};
    ks::engine::test::Client client_a;
    REQUIRE(a->attach(client_a.ring()) == std::optional<std::size_t>{0});
    REQUIRE(a->load(0, track->track()));
    REQUIRE(client_a.send(ks::engine::test::playing(0, true)));
    const Clock::time_point start = ks::engine::resume_test::kStart;
    REQUIRE(render_blocks(core_a, channels.views(), start, 10) == 10);

    // Engine B resumes from a copy of its state block, 100 ms later, and takes
    // a command in its first block.
    auto copy = ks::engine::resume_test::copy_of(*state);
    auto b = std::make_unique<ks::engine::Engine>(*copy, tracks);
    RenderCore core_b{*b};
    ks::engine::test::Client client_b;
    REQUIRE(b->attach(client_b.ring()) == std::optional<std::size_t>{0});
    REQUIRE(client_b.send(ks::engine::test::deck_command(ks::ipc::CommandType::SetRate, 0, 1.02)));
    const Clock::time_point resumed =
        start + ks::engine::frames_to_duration(10 * kBlock, kRate) + 100ms;
    REQUIRE(render_blocks(core_b, channels.views(), resumed, 200) == 200);

    REQUIRE(b->mixer().deck(0).track() == track->get());
    REQUIRE(b->mixer().deck(0).playing());
    REQUIRE(b->mixer().deck(0).rate() == 1.02);
    // Generations 1 to 10 were A's; B wrote 11 to 210.
    REQUIRE(ks::engine::resume_test::slot_generation(*copy, 0) == 210);
}
