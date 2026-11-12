// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/state-out/spec.md: real-time safety
// (requirement 20).

#include <ks/core/realtime.h>

#include <ks/engine/block.h>
#include <ks/engine/engine.h>
#include <ks/engine/render_core.h>
#include <ks/ipc/state.h>

#include "../commands/test_support.h"
#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace {

using ks::engine::Block;
using ks::engine::RenderCore;
using ks::engine::RenderResult;
using ks::engine::state_test::kBlock;
using ks::engine::state_test::kRate;

// Every call here, and everything try_render() does inside, the engine's
// publishing included, is checked by function effect analysis at compile
// time, and by RealtimeSanitizer at run time under the rtsan preset.
std::uint64_t render_blocks(RenderCore& core, std::span<const std::span<float>> channels,
                            std::size_t count) noexcept KS_NONBLOCKING {
    std::uint64_t rendered = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const Block block{.channels = channels,
                          .frames = kBlock,
                          .sample_rate = kRate,
                          .host_time = {},
                          .output_latency = {},
                          .resumed = false};
        rendered += core.try_render(block) == RenderResult::Rendered ? 1 : 0;
    }
    return rendered;
}

} // namespace

TEST_CASE("State out is real-time safe", "[state][realtime]") {
    auto state = std::make_unique<ks::ipc::EngineState>();
    auto engine = std::make_unique<ks::engine::Engine>(*state);
    RenderCore core{*engine};
    ks::engine::test::Client client;
    ks::engine::state_test::IdTrack track{48'000, 5};
    REQUIRE(engine->attach(client.ring()) == std::optional<std::size_t>{0});
    REQUIRE(engine->load(0, track.track()));
    REQUIRE(client.send(ks::engine::test::playing(0, true)));

    const ks::engine::state_test::Channels channels;
    REQUIRE(render_blocks(core, channels.views(), 10) == 10);

    using ks::engine::state_test::read;
    REQUIRE(read(state->status).heartbeat == 10);
    const ks::ipc::DeckState deck = read(state->decks.at(0));
    REQUIRE(deck.track_id == 5);
    REQUIRE((deck.flags & ks::ipc::kDeckPlaying) != 0);
}
