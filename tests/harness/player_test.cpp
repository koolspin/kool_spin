// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the player both modes share (specs/harness/spec.md
// requirements 18 to 20 and 27).

#include "player.h"

#include <ks/engine/offline.h>
#include <ks/engine/render_core.h>
#include <ks/harness/run.h>
#include <ks/harness/script.h>
#include <ks/harness/tracks.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using ks::harness::event_frame;
using ks::harness::Player;
using ks::harness::read_script;
using ks::harness::Report;
using ks::harness::Script;
using ks::harness::ScriptError;
using ks::harness::TrackSet;

constexpr std::uint32_t kRate = 48'000;
constexpr std::size_t kBlock = 256;

Script script_of(const std::string& text) {
    std::expected<Script, ScriptError> script = read_script(text, "/");
    REQUIRE(script.has_value());
    return std::move(*script);
}

TrackSet tracks_of(const Script& script) {
    std::expected<TrackSet, std::string> tracks = TrackSet::prepare(script);
    REQUIRE(tracks.has_value());
    return std::move(*tracks);
}

std::unique_ptr<Player> player_of(const Script& script, const TrackSet& tracks) {
    std::expected<std::unique_ptr<Player>, std::string> player = Player::create(script, tracks);
    REQUIRE(player.has_value());
    return std::move(*player);
}

/// Renders one block of kBlock stereo frames through `core`, then collects.
void render_block(ks::engine::RenderCore& core, Player& player) {
    std::vector<float> left(kBlock);
    std::vector<float> right(kBlock);
    const std::array<std::span<float>, 2> output{std::span{left}, std::span{right}};
    const ks::engine::OfflineConfig config{
        .sample_rate = kRate, .block_frames = kBlock, .start_time = {}};
    REQUIRE(ks::engine::render_offline(core, config, output) == kBlock);
    player.collect();
}

} // namespace

TEST_CASE("Player: event frames round to the nearest frame", "[harness]") {
    CHECK(event_frame(0.0, kRate) == 0);
    CHECK(event_frame(0.5, kRate) == 24'000);
    CHECK(event_frame(0.1234, 44'100) == 5'442); // 5441.94
    CHECK(event_frame(0.0001, kRate) == 5);      // 4.8
    CHECK(event_frame(86'400.0, 384'000) == 33'177'600'000);
}

TEST_CASE("Player: gives events as they fall due, and the engine reads them", "[harness]") {
    const Script script = script_of("track t sine 1000 1 rate 32000\n"
                                    "0 load 1 t\n"
                                    "0 play 1\n"
                                    "0.0001 seek 1 0.5\n"
                                    "0.01 rate 1 -1\n"
                                    "0.1 end\n");
    const TrackSet tracks = tracks_of(script);
    const std::unique_ptr<Player> player = player_of(script, tracks);
    ks::engine::RenderCore core{player->engine()};

    REQUIRE(player->next_time() == 0.0);
    player->give_due(0.0);
    CHECK(player->commands_sent() == 1); // a load isn't a command
    REQUIRE(player->next_time() == 0.0001);

    // The seek is due at frame 5, not 4.
    player->give_due_at_frame(4, kRate);
    CHECK(player->commands_sent() == 1);
    player->give_due_at_frame(5, kRate);
    CHECK(player->commands_sent() == 2);
    REQUIRE(player->next_time() == 0.01);

    render_block(core, *player);
    CHECK(player->all_read());
    player->give_due(0.01);
    CHECK(player->commands_sent() == 3);
    CHECK(!player->next_time().has_value());
    CHECK(!player->all_read());
    render_block(core, *player);
    CHECK(player->all_read());

    Report report;
    player->fill(report);
    CHECK(report.played_seconds == static_cast<double>(2 * kBlock) / kRate);
    CHECK(report.blocks_rendered == 2);
    CHECK(report.commands_sent == 3);
    CHECK(report.commands_read == 3);
    CHECK(report.commands_rejected == 0);
    CHECK(report.events_delayed == 0);
    CHECK(report.non_finite_samples == 0);

    // The deck took the track, played it, and went into reverse.
    const ks::engine::Deck& deck = player->engine().mixer().deck(0);
    CHECK(deck.track() == &tracks.track(0));
    CHECK(deck.playing());
    CHECK(deck.rate() == -1.0);
}

TEST_CASE("Player: events wait while the command ring is full", "[harness]") {
    std::string text = "track t sine 1000 1 rate 48000\n0 load 1 t\n";
    for (int i = 0; i < 300; ++i) {
        text += "0 fader 1 0.5\n";
    }
    text += "0 unload 1\n0.5 play 1\n1 end\n";
    const Script script = script_of(text);
    const TrackSet tracks = tracks_of(script);
    const std::unique_ptr<Player> player = player_of(script, tracks);
    ks::engine::RenderCore core{player->engine()};

    player->give_due(0.0);
    const std::uint64_t first = player->commands_sent();
    CHECK(first > 0);
    CHECK(first < 300);
    // The commands left, and the unload behind them, are delayed, and the
    // event at 0.5 s isn't due yet.
    CHECK(player->events_delayed() == 300 - first + 1);

    // Retrying gives the rest, in order, and counts nothing twice.
    int blocks = 0;
    while (player->next_time() == 0.0 && blocks < 100) {
        render_block(core, *player);
        ++blocks;
        player->give_due(0.0);
    }
    CHECK(player->next_time() == 0.5);
    CHECK(player->commands_sent() == 300);
    CHECK(player->events_delayed() == 300 - first + 1);
    // The engine reads 32 commands a block, so the ring takes a few blocks to
    // drain.
    for (blocks = 0; !player->all_read() && blocks < 100; ++blocks) {
        render_block(core, *player);
    }

    Report report;
    player->fill(report);
    CHECK(report.commands_sent == 300);
    CHECK(report.commands_read == 300);
    CHECK(report.commands_rejected == 0);
    CHECK(report.events_delayed == 300 - first + 1);
    CHECK(player->engine().mixer().deck(0).track() == nullptr);
}

TEST_CASE("Player: refuses tracks prepared from another script", "[harness]") {
    const Script one = script_of("track a sine 1000 1\n0 load 1 a\n1 end\n");
    const Script two = script_of("track a sine 1000 1\ntrack b noise 1\n0 load 1 b\n1 end\n");
    const TrackSet tracks = tracks_of(one);
    CHECK(!Player::create(two, tracks).has_value());
}
