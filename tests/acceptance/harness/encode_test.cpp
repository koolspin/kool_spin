// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/harness/spec.md: encoding commands
// (requirement 17).

#include <ks/harness/encode.h>

#include <ks/engine/command.h>
#include <ks/engine/mixer.h>
#include <ks/ipc/command.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <vector>

namespace {

using ks::engine::BusMap;
using ks::engine::Command;
using ks::engine::CrossfaderSide;
using ks::engine::decode;
using ks::engine::Rejection;
using ks::harness::encode;
using ks::ipc::CommandSlot;
using ks::ipc::CommandType;

constexpr double kMax = std::numeric_limits<double>::max();
constexpr double kTiny = std::numeric_limits<double>::denorm_min();
constexpr double kMinusInfinity = -std::numeric_limits<double>::infinity();

/// Every command type, with values at its edges, as decode() returns them.
std::vector<Command> edge_commands() {
    std::vector<Command> commands;
    for (const std::size_t deck : {std::size_t{0}, std::size_t{3}}) {
        for (const bool on : {false, true}) {
            commands.push_back(Command{.type = CommandType::SetPlaying, .deck = deck, .on = on});
            commands.push_back(Command{.type = CommandType::SetChannelCue, .deck = deck, .on = on});
        }
        for (const CrossfaderSide side :
             {CrossfaderSide::A, CrossfaderSide::B, CrossfaderSide::Thru}) {
            commands.push_back(
                Command{.type = CommandType::SetCrossfaderAssign, .deck = deck, .side = side});
        }
        for (const double value : {-kMax, -4.0, -1.0, 0.0, kTiny, 1.0, 4.0, kMax}) {
            commands.push_back(Command{.type = CommandType::SetRate, .deck = deck, .value = value});
            commands.push_back(Command{.type = CommandType::Seek, .deck = deck, .value = value});
            commands.push_back(
                Command{.type = CommandType::SetCuePoint, .deck = deck, .value = value});
            commands.push_back(Command{.type = CommandType::SetTrim, .deck = deck, .value = value});
            commands.push_back(
                Command{.type = CommandType::SetFader, .deck = deck, .value = value});
        }
        commands.push_back(
            Command{.type = CommandType::SetTrim, .deck = deck, .value = kMinusInfinity});
    }
    for (const double value : {-kMax, -1.0, 0.0, kTiny, 0.5, 1.0, kMax}) {
        commands.push_back(Command{.type = CommandType::SetCrossfader, .value = value});
        commands.push_back(Command{.type = CommandType::SetCrossfaderCurve, .value = value});
        commands.push_back(Command{.type = CommandType::SetMainLevel, .value = value});
        commands.push_back(Command{.type = CommandType::SetHeadphoneLevel, .value = value});
        commands.push_back(Command{.type = CommandType::SetCueMix, .value = value});
    }
    commands.push_back(Command{.type = CommandType::SetMainLevel, .value = kMinusInfinity});
    commands.push_back(Command{.type = CommandType::SetHeadphoneLevel, .value = kMinusInfinity});
    for (const BusMap map : {BusMap{.main = 0, .cue = 2}, BusMap{.main = 2, .cue = 0},
                             BusMap{.main = 6, .cue = 4}, BusMap{.main = 0, .cue = 6}}) {
        commands.push_back(Command{.type = CommandType::SetBusMap, .bus_map = map});
    }
    return commands;
}

} // namespace

TEST_CASE("Encoder: every command survives decode()", "[harness]") {
    std::uint64_t sequence = 1;
    std::size_t types_seen = 0;
    std::vector<bool> seen(16, false);
    for (const Command& command : edge_commands()) {
        CAPTURE(static_cast<int>(command.type), command.deck, command.value, command.on);
        const CommandSlot slot = encode(command, sequence);
        CHECK(slot.sequence == sequence);
        const std::expected<Command, Rejection> decoded = decode(slot);
        REQUIRE(decoded.has_value());
        CHECK(*decoded == command);

        const auto type = static_cast<std::size_t>(command.type);
        if (!seen.at(type)) {
            seen.at(type) = true;
            ++types_seen;
        }
        sequence = (sequence * 3) + 1; // all sorts of sequence numbers
    }
    CHECK(types_seen == 14); // every CommandType
}
