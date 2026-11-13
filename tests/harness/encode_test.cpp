// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the harness's command encoder.

#include <ks/harness/encode.h>

#include <ks/engine/command.h>
#include <ks/engine/mixer.h>
#include <ks/ipc/command.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace {

using ks::engine::BusMap;
using ks::engine::Command;
using ks::engine::CrossfaderSide;
using ks::harness::encode;
using ks::ipc::CommandType;

TEST_CASE("encode: the slot's layout") {
    SECTION("sequence, type, and target") {
        const auto slot = encode(Command{.type = CommandType::SetRate, .deck = 2, .value = 1.5}, 7);
        CHECK(slot.sequence == 7);
        CHECK(slot.type == 2);
        CHECK(slot.target == 2);
        CHECK(slot.payload[0] == std::bit_cast<std::uint64_t>(1.5));
        for (std::size_t i = 1; i < slot.payload.size(); ++i) {
            CHECK(slot.payload.at(i) == 0);
        }
    }
    SECTION("global commands target 0, whatever the deck field says") {
        const auto slot =
            encode(Command{.type = CommandType::SetCrossfader, .deck = 3, .value = 0.5}, 1);
        CHECK(slot.target == 0);
    }
    SECTION("on/off and sides") {
        CHECK(encode(Command{.type = CommandType::SetPlaying, .on = true}, 1).payload[0] == 1);
        CHECK(encode(Command{.type = CommandType::SetChannelCue, .on = false}, 1).payload[0] == 0);
        CHECK(
            encode(Command{.type = CommandType::SetCrossfaderAssign, .side = CrossfaderSide::B}, 1)
                .payload[0] == 1);
    }
    SECTION("bus map") {
        const auto slot = encode(
            Command{.type = CommandType::SetBusMap, .bus_map = BusMap{.main = 4, .cue = 6}}, 1);
        CHECK(slot.payload[0] == 4);
        CHECK(slot.payload[1] == 6);
        CHECK(slot.payload[2] == 0);
    }
}

TEST_CASE("encode: round trip through decode()") {
    const std::array<Command, 6> commands{{
        {.type = CommandType::SetPlaying, .deck = 1, .on = true},
        {.type = CommandType::Seek, .deck = 3, .value = 44100.0},
        {.type = CommandType::SetTrim, .deck = 0, .value = -6.0},
        {.type = CommandType::SetCrossfaderAssign, .deck = 2, .side = CrossfaderSide::A},
        {.type = CommandType::SetCrossfaderCurve, .value = 0.25},
        {.type = CommandType::SetBusMap, .bus_map = BusMap{.main = 2, .cue = 0}},
    }};
    for (const Command& command : commands) {
        const auto decoded = ks::engine::decode(encode(command, 1));
        REQUIRE(decoded.has_value());
        CHECK(*decoded == command);
    }
}

} // namespace
