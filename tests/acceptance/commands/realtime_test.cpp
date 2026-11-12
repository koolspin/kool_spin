// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/commands/spec.md: real-time safety.

#include <ks/core/realtime.h>

#include <ks/engine/block.h>
#include <ks/engine/command.h>
#include <ks/engine/engine.h>
#include <ks/engine/mixer.h>
#include <ks/ipc/command.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace {

using ks::engine::Block;
using ks::engine::Engine;
using ks::engine::Mixer;
using ks::engine::test::kRate;

constexpr std::size_t kFrames = 64;

// Every call here, and everything process() does inside, is checked by
// function effect analysis at compile time, and by RealtimeSanitizer at run
// time under the rtsan preset.
std::uint64_t run_a_block(Engine& engine, Mixer& mixer, const ks::ipc::CommandSlot& slot,
                          std::span<const std::span<float>> channels) noexcept KS_NONBLOCKING {
    const auto command = ks::engine::decode(slot);
    if (command.has_value()) {
        ks::engine::apply(*command, mixer);
    }
    const Block block{.channels = channels,
                      .frames = kFrames,
                      .sample_rate = kRate,
                      .host_time = {},
                      .resumed = false};
    engine.process(block);
    return engine.client_stats(0).commands_read + (engine.mixer().deck(0).playing() ? 100U : 0U);
}

} // namespace

TEST_CASE("Commands are real-time safe", "[commands][realtime]") {
    using ks::engine::test::encode;
    using ks::engine::test::playing;

    auto engine = std::make_unique<Engine>();
    ks::engine::test::Client client;
    const auto track = ks::engine::test::constant_track(0.25F);
    REQUIRE(engine->attach(client.ring()) == std::optional<std::size_t>{0});
    REQUIRE(engine->load(0, track->track()));
    REQUIRE(client.send(playing(0, true)));

    ks::engine::test::Rig rig;
    std::array<std::vector<float>, 4> storage{
        std::vector<float>(kFrames), std::vector<float>(kFrames), std::vector<float>(kFrames),
        std::vector<float>(kFrames)};
    const std::array<std::span<float>, 4> channels{std::span{storage[0]}, std::span{storage[1]},
                                                   std::span{storage[2]}, std::span{storage[3]}};

    // The engine read the play command and started deck 0; the command decoded
    // here started the rig's deck 1.
    REQUIRE(run_a_block(*engine, rig.mixer, encode(1, playing(1, true)), channels) == 101);
    REQUIRE(rig.mixer.deck(1).playing());
    REQUIRE(storage[0].back() > 0.0F);
}
