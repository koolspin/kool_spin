// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/commands/spec.md: the command slot and the
// decoder.

#include <ks/engine/command.h>
#include <ks/engine/mixer.h>
#include <ks/ipc/command.h>

#include "codec_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <type_traits>
#include <vector>

namespace {

using ks::engine::BusMap;
using ks::engine::Command;
using ks::engine::CrossfaderSide;
using ks::engine::decode;
using ks::engine::Rejection;
using ks::engine::test::bits_of;
using ks::ipc::CommandSlot;
using ks::ipc::CommandType;

constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr std::uint64_t kQuietNaN = 0x7FF8'0000'0000'0000ULL;
constexpr std::uint64_t kSignallingNaN = 0x7FF0'0000'0000'0001ULL;
constexpr std::uint64_t kNegativeNaN = 0xFFF8'0000'0000'0000ULL;

/// A slot built by hand, field by field.
CommandSlot slot(std::uint32_t type, std::uint32_t target, std::uint64_t first,
                 std::uint64_t second = 0) {
    CommandSlot slot{};
    slot.sequence = 0x0123'4567'89AB'CDEFULL;
    slot.type = type;
    slot.target = target;
    slot.payload[0] = first;
    slot.payload[1] = second;
    return slot;
}

CommandSlot slot(CommandType type, std::uint32_t target, std::uint64_t first,
                 std::uint64_t second = 0) {
    return slot(static_cast<std::uint32_t>(type), target, first, second);
}

struct Accepted {
    CommandSlot slot;
    Command expected;
};

struct Refused {
    CommandSlot slot;
    Rejection reason;
};

/// The ten types that carry a double.
constexpr std::array<CommandType, 10> kDoubleTypes{
    CommandType::SetRate,
    CommandType::Seek,
    CommandType::SetCuePoint,
    CommandType::SetTrim,
    CommandType::SetFader,
    CommandType::SetCrossfader,
    CommandType::SetCrossfaderCurve,
    CommandType::SetMainLevel,
    CommandType::SetHeadphoneLevel,
    CommandType::SetCueMix,
};

/// A target that's valid for `type`.
std::uint32_t valid_target(CommandType type) {
    return ks::engine::test::takes_deck(type) ? 3 : 0;
}

} // namespace

TEST_CASE("Commands: the slot layout", "[commands]") {
    STATIC_REQUIRE(sizeof(CommandSlot) == 64);
    STATIC_REQUIRE(std::is_standard_layout_v<CommandSlot>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<CommandSlot>);
    STATIC_REQUIRE(offsetof(CommandSlot, sequence) == 0);
    STATIC_REQUIRE(offsetof(CommandSlot, type) == 8);
    STATIC_REQUIRE(offsetof(CommandSlot, target) == 12);
    STATIC_REQUIRE(offsetof(CommandSlot, payload) == 16);
    STATIC_REQUIRE(std::is_same_v<decltype(CommandSlot::payload), std::array<std::uint64_t, 6>>);
    STATIC_REQUIRE(ks::ipc::kCommandRingCapacity == 256);
    STATIC_REQUIRE(ks::ipc::CommandRing::kCapacity == 256);

    // A zero-filled slot is no command.
    const auto zero = decode(CommandSlot{});
    REQUIRE(zero.error_or(Rejection::BadValue) == Rejection::UnknownType);
}

TEST_CASE("Commands: every type decodes", "[commands]") {
    using enum CommandType;
    const std::vector<Accepted> cases{
        {slot(SetPlaying, 2, 1), {.type = SetPlaying, .deck = 2, .on = true}},
        {slot(SetPlaying, 0, 0), {.type = SetPlaying, .deck = 0, .on = false}},
        {slot(SetRate, 1, bits_of(1.06)), {.type = SetRate, .deck = 1, .value = 1.06}},
        {slot(SetRate, 1, bits_of(10.0)), {.type = SetRate, .deck = 1, .value = 10.0}},
        {slot(SetRate, 0, bits_of(-1.0)), {.type = SetRate, .deck = 0, .value = -1.0}},
        {slot(Seek, 3, bits_of(12'345.5)), {.type = Seek, .deck = 3, .value = 12'345.5}},
        {slot(Seek, 3, bits_of(-5.0)), {.type = Seek, .deck = 3, .value = -5.0}},
        {slot(SetCuePoint, 0, bits_of(480.0)), {.type = SetCuePoint, .deck = 0, .value = 480.0}},
        {slot(SetTrim, 1, bits_of(-6.0)), {.type = SetTrim, .deck = 1, .value = -6.0}},
        {slot(SetTrim, 1, bits_of(20.0)), {.type = SetTrim, .deck = 1, .value = 20.0}},
        {slot(SetTrim, 2, bits_of(-kInfinity)), {.type = SetTrim, .deck = 2, .value = -kInfinity}},
        {slot(SetFader, 2, bits_of(0.5)), {.type = SetFader, .deck = 2, .value = 0.5}},
        {slot(SetFader, 2, bits_of(2.0)), {.type = SetFader, .deck = 2, .value = 2.0}},
        {slot(SetCrossfaderAssign, 0, 0),
         {.type = SetCrossfaderAssign, .deck = 0, .side = CrossfaderSide::A}},
        {slot(SetCrossfaderAssign, 1, 1),
         {.type = SetCrossfaderAssign, .deck = 1, .side = CrossfaderSide::B}},
        {slot(SetCrossfaderAssign, 3, 2),
         {.type = SetCrossfaderAssign, .deck = 3, .side = CrossfaderSide::Thru}},
        {slot(SetChannelCue, 3, 1), {.type = SetChannelCue, .deck = 3, .on = true}},
        {slot(SetChannelCue, 1, 0), {.type = SetChannelCue, .deck = 1, .on = false}},
        {slot(SetCrossfader, 0, bits_of(0.25)), {.type = SetCrossfader, .value = 0.25}},
        {slot(SetCrossfader, 0, bits_of(-1.0)), {.type = SetCrossfader, .value = -1.0}},
        {slot(SetCrossfaderCurve, 0, bits_of(0.05)), {.type = SetCrossfaderCurve, .value = 0.05}},
        {slot(SetMainLevel, 0, bits_of(-3.0)), {.type = SetMainLevel, .value = -3.0}},
        {slot(SetMainLevel, 0, bits_of(-kInfinity)), {.type = SetMainLevel, .value = -kInfinity}},
        {slot(SetHeadphoneLevel, 0, bits_of(-1.5)), {.type = SetHeadphoneLevel, .value = -1.5}},
        {slot(SetHeadphoneLevel, 0, bits_of(-kInfinity)),
         {.type = SetHeadphoneLevel, .value = -kInfinity}},
        {slot(SetCueMix, 0, bits_of(0.5)), {.type = SetCueMix, .value = 0.5}},
        {slot(SetBusMap, 0, 2, 0), {.type = SetBusMap, .bus_map = BusMap{.main = 2, .cue = 0}}},
        {slot(SetBusMap, 0, 4, 6), {.type = SetBusMap, .bus_map = BusMap{.main = 4, .cue = 6}}},
    };

    for (std::size_t i = 0; i < cases.size(); ++i) {
        const Accepted& accepted = cases.at(i);
        CAPTURE(i, accepted.slot.type);
        const auto decoded = decode(accepted.slot);
        REQUIRE(decoded.has_value());
        const Command command = decoded.value_or(Command{});
        REQUIRE(command == accepted.expected);
        REQUIRE(bits_of(command.value) == bits_of(accepted.expected.value));
    }
}

TEST_CASE("Commands: invalid slots are rejected", "[commands]") {
    using enum CommandType;
    std::vector<Refused> cases{
        // The type.
        {slot(0, 0, 0), Rejection::UnknownType},
        {slot(15, 0, 0), Rejection::UnknownType},
        {slot(0xFFFF'FFFFU, 0, 0), Rejection::UnknownType},
        // The target.
        {slot(SetRate, 4, bits_of(1.0)), Rejection::BadTarget},
        {slot(SetPlaying, 4, 1), Rejection::BadTarget},
        {slot(SetFader, 4, bits_of(1.0)), Rejection::BadTarget},
        {slot(SetChannelCue, 0xFFFF'FFFFU, 1), Rejection::BadTarget},
        {slot(SetCrossfader, 1, bits_of(0.5)), Rejection::BadTarget},
        {slot(SetBusMap, 1, 0, 2), Rejection::BadTarget},
        // Padding.
        {slot(SetRate, 0, bits_of(1.0), 1), Rejection::NonzeroPadding},
        {slot(SetPlaying, 0, 1, 1), Rejection::NonzeroPadding},
        {slot(SetCrossfader, 0, bits_of(0.5), 0x8000'0000'0000'0000ULL), Rejection::NonzeroPadding},
        // Flags, sides, and bus maps.
        {slot(SetPlaying, 0, 2), Rejection::BadValue},
        {slot(SetChannelCue, 0, 2), Rejection::BadValue},
        {slot(SetCrossfaderAssign, 0, 3), Rejection::BadValue},
        {slot(SetBusMap, 0, 1, 2), Rejection::BadValue},
        {slot(SetBusMap, 0, 2, 2), Rejection::BadValue},
        {slot(SetBusMap, 0, 8, 0), Rejection::BadValue},
        {slot(SetBusMap, 0, 0, std::uint64_t{1} << 40U), Rejection::BadValue},
        // Several faults: the first in requirement 3's order is reported.
        {slot(0, 9, 0, 1), Rejection::UnknownType},
        {slot(SetRate, 4, kQuietNaN, 1), Rejection::BadTarget},
        {slot(SetRate, 0, kQuietNaN, 1), Rejection::NonzeroPadding},
    };
    // Padding beyond payload[1], in every word.
    for (std::size_t word = 2; word < 6; ++word) {
        CommandSlot padded = slot(SetBusMap, 0, 0, 2);
        padded.payload.at(word) = 1;
        cases.push_back({padded, Rejection::NonzeroPadding});
    }
    // Doubles: NaN and +∞ everywhere; −∞ except where it's allowed.
    for (const CommandType type : kDoubleTypes) {
        const std::uint32_t target = valid_target(type);
        for (const std::uint64_t nan : {kQuietNaN, kSignallingNaN, kNegativeNaN}) {
            cases.push_back({slot(type, target, nan), Rejection::BadValue});
        }
        cases.push_back({slot(type, target, bits_of(kInfinity)), Rejection::BadValue});
        if (!ks::engine::test::allows_minus_infinity(type)) {
            cases.push_back({slot(type, target, bits_of(-kInfinity)), Rejection::BadValue});
        }
    }

    for (std::size_t i = 0; i < cases.size(); ++i) {
        const Refused& refused = cases.at(i);
        CAPTURE(i, refused.slot.type, refused.slot.target, refused.slot.payload[0]);
        const auto decoded = decode(refused.slot);
        const bool accepted = decoded.has_value();
        REQUIRE(!accepted);
        REQUIRE(decoded.error_or(Rejection::UnknownType) == refused.reason);
        // The same slot with a valid sequence number, or any other, fails
        // the same way: the sequence number isn't validated.
        CommandSlot resequenced = refused.slot;
        resequenced.sequence = 0;
        REQUIRE(decode(resequenced).error_or(Rejection::UnknownType) == refused.reason);
    }
}

TEST_CASE("Commands: decoding and encoding round-trip", "[commands]") {
    using ks::engine::test::ByteReader;
    using ks::engine::test::command_round_trips;
    using ks::engine::test::encode;
    using ks::engine::test::slot_round_trips;
    using ks::engine::test::valid_command;

    std::mt19937_64 random{20'261'111};
    const auto random_bytes = [&random](std::span<std::uint8_t> bytes) {
        for (std::uint8_t& byte : bytes) {
            byte = static_cast<std::uint8_t>(random() & 0xFFU);
        }
    };

    std::size_t decoded_valid = 0;
    for (std::size_t i = 0; i < 1'000'000; ++i) {
        // Random bytes.
        std::array<std::uint8_t, sizeof(CommandSlot)> bytes{};
        random_bytes(bytes);
        const auto from_bytes = std::bit_cast<CommandSlot>(bytes);
        if (!slot_round_trips(from_bytes)) {
            FAIL("random slot " << i << " decoded, but didn't encode back to the same bytes");
        }

        // A valid command, there and back.
        std::array<std::uint8_t, 16> choice{};
        random_bytes(choice);
        ByteReader reader{choice};
        const Command command = valid_command(reader);
        const std::uint64_t sequence = random();
        if (!command_round_trips(sequence, command)) {
            FAIL("valid command " << i << " (type " << static_cast<std::uint32_t>(command.type)
                                  << ") didn't decode back to itself");
        }

        // The same command with one byte changed.
        auto changed =
            std::bit_cast<std::array<std::uint8_t, sizeof(CommandSlot)>>(encode(sequence, command));
        changed.at(random() % changed.size()) = static_cast<std::uint8_t>(random() & 0xFFU);
        const auto mutated = std::bit_cast<CommandSlot>(changed);
        if (decode(mutated).has_value()) {
            ++decoded_valid;
        }
        if (!slot_round_trips(mutated)) {
            FAIL("mutated slot " << i << " decoded, but didn't encode back to the same bytes");
        }
    }
    // Many mutations hit the sequence number or leave a valid command.
    REQUIRE(decoded_valid > 100'000);
}
