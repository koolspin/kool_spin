// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/command.h>

#include <ks/engine/mixer.h>
#include <ks/ipc/command.h>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

using ks::engine::BusMap;
using ks::engine::Command;
using ks::engine::CrossfaderSide;
using ks::engine::decode;
using ks::engine::Rejection;
using ks::ipc::CommandSlot;
using ks::ipc::CommandType;

CommandSlot slot_of(std::uint32_t type, std::uint32_t target, std::uint64_t argument,
                    std::uint64_t second = 0) {
    CommandSlot slot{};
    slot.sequence = 7;
    slot.type = type;
    slot.target = target;
    slot.payload[0] = argument;
    slot.payload[1] = second;
    return slot;
}

CommandSlot slot_of(CommandType type, std::uint32_t target, std::uint64_t argument,
                    std::uint64_t second = 0) {
    return slot_of(static_cast<std::uint32_t>(type), target, argument, second);
}

std::uint64_t bits(double value) {
    return std::bit_cast<std::uint64_t>(value);
}

constexpr double kInfinity = std::numeric_limits<double>::infinity();

Rejection rejection_of(const CommandSlot& slot) {
    const auto result = decode(slot);
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

TEST_CASE("decode: one valid slot per type", "[command]") {
    using enum CommandType;

    CHECK(decode(slot_of(SetPlaying, 3, 1)) == Command{.type = SetPlaying, .deck = 3, .on = true});
    CHECK(decode(slot_of(SetRate, 1, bits(1.02))) ==
          Command{.type = SetRate, .deck = 1, .value = 1.02});
    CHECK(decode(slot_of(Seek, 2, bits(4800.5))) ==
          Command{.type = Seek, .deck = 2, .value = 4800.5});
    CHECK(decode(slot_of(SetCuePoint, 0, bits(-5.0))) ==
          Command{.type = SetCuePoint, .deck = 0, .value = -5.0});
    CHECK(decode(slot_of(SetTrim, 2, bits(-kInfinity))) ==
          Command{.type = SetTrim, .deck = 2, .value = -kInfinity});
    CHECK(decode(slot_of(SetFader, 3, bits(2.0))) ==
          Command{.type = SetFader, .deck = 3, .value = 2.0});
    CHECK(decode(slot_of(SetCrossfaderAssign, 1, 0)) ==
          Command{.type = SetCrossfaderAssign, .deck = 1, .side = CrossfaderSide::A});
    CHECK(decode(slot_of(SetCrossfaderAssign, 1, 1)) ==
          Command{.type = SetCrossfaderAssign, .deck = 1, .side = CrossfaderSide::B});
    CHECK(decode(slot_of(SetCrossfaderAssign, 1, 2)) ==
          Command{.type = SetCrossfaderAssign, .deck = 1, .side = CrossfaderSide::Thru});
    CHECK(decode(slot_of(SetChannelCue, 0, 0)) ==
          Command{.type = SetChannelCue, .deck = 0, .on = false});
    CHECK(decode(slot_of(SetCrossfader, 0, bits(0.25))) ==
          Command{.type = SetCrossfader, .value = 0.25});
    CHECK(decode(slot_of(SetCrossfaderCurve, 0, bits(0.1))) ==
          Command{.type = SetCrossfaderCurve, .value = 0.1});
    CHECK(decode(slot_of(SetMainLevel, 0, bits(-kInfinity))) ==
          Command{.type = SetMainLevel, .value = -kInfinity});
    CHECK(decode(slot_of(SetHeadphoneLevel, 0, bits(-6.0))) ==
          Command{.type = SetHeadphoneLevel, .value = -6.0});
    CHECK(decode(slot_of(SetCueMix, 0, bits(0.5))) == Command{.type = SetCueMix, .value = 0.5});
    CHECK(decode(slot_of(SetBusMap, 0, 6, 0)) ==
          Command{.type = SetBusMap, .bus_map = BusMap{.main = 6, .cue = 0}});
}

TEST_CASE("decode: −0.0 and the largest finite double are accepted", "[command]") {
    const auto negative_zero = decode(slot_of(CommandType::SetRate, 0, bits(-0.0)));
    REQUIRE(negative_zero.has_value());
    CHECK(std::signbit(negative_zero->value));
    CHECK(bits(negative_zero->value) == bits(-0.0));

    const double largest = std::numeric_limits<double>::max();
    CHECK(decode(slot_of(CommandType::Seek, 0, bits(largest)))->value == largest);
    CHECK(decode(slot_of(CommandType::Seek, 0, bits(-largest)))->value == -largest);
}

TEST_CASE("decode: unknown types", "[command]") {
    for (const std::uint32_t type : {0U, 15U, 0x101U, 0x10EU, 0xFFFF'FFFFU}) {
        CAPTURE(type);
        CHECK(rejection_of(slot_of(type, 0, 0)) == Rejection::UnknownType);
    }
    CHECK(rejection_of(CommandSlot{}) == Rejection::UnknownType);
}

TEST_CASE("decode: bad targets", "[command]") {
    CHECK(rejection_of(slot_of(CommandType::SetPlaying, 4, 0)) == Rejection::BadTarget);
    CHECK(rejection_of(slot_of(CommandType::SetFader, 4, 0)) == Rejection::BadTarget);
    CHECK(rejection_of(slot_of(CommandType::SetFader, 0xFFFF'FFFF, 0)) == Rejection::BadTarget);
    CHECK(rejection_of(slot_of(CommandType::SetCrossfader, 1, 0)) == Rejection::BadTarget);
    CHECK(rejection_of(slot_of(CommandType::SetBusMap, 1, 0, 2)) == Rejection::BadTarget);
}

TEST_CASE("decode: nonzero padding", "[command]") {
    CHECK(rejection_of(slot_of(CommandType::SetRate, 0, bits(1.0), 1)) ==
          Rejection::NonzeroPadding);
    for (std::size_t word = 2; word < 6; ++word) {
        CAPTURE(word);
        auto slot = slot_of(CommandType::SetBusMap, 0, 0, 2);
        slot.payload.at(word) = 1;
        CHECK(rejection_of(slot) == Rejection::NonzeroPadding);
    }
}

TEST_CASE("decode: bad values", "[command]") {
    using enum CommandType;
    const std::uint64_t quiet_nan = bits(std::numeric_limits<double>::quiet_NaN());
    const std::uint64_t signalling_nan = 0x7FF0'0000'0000'0001;
    const std::uint64_t negative_nan = quiet_nan | 0x8000'0000'0000'0000;

    for (const auto type : {SetRate, Seek, SetCuePoint, SetTrim, SetFader, SetCrossfader,
                            SetCrossfaderCurve, SetMainLevel, SetHeadphoneLevel, SetCueMix}) {
        CAPTURE(type);
        for (const std::uint64_t value :
             {quiet_nan, signalling_nan, negative_nan, bits(kInfinity)}) {
            CHECK(rejection_of(slot_of(type, 0, value)) == Rejection::BadValue);
        }
        const bool minus_infinity_allowed =
            type == SetTrim || type == SetMainLevel || type == SetHeadphoneLevel;
        CHECK(decode(slot_of(type, 0, bits(-kInfinity))).has_value() == minus_infinity_allowed);
    }

    CHECK(rejection_of(slot_of(SetPlaying, 0, 2)) == Rejection::BadValue);
    CHECK(rejection_of(slot_of(SetChannelCue, 0, 0x1'0000'0001)) == Rejection::BadValue);
    CHECK(rejection_of(slot_of(SetCrossfaderAssign, 0, 3)) == Rejection::BadValue);
    CHECK(rejection_of(slot_of(SetBusMap, 0, 1, 2)) == Rejection::BadValue);
    CHECK(rejection_of(slot_of(SetBusMap, 0, 2, 2)) == Rejection::BadValue);
    CHECK(rejection_of(slot_of(SetBusMap, 0, 8, 0)) == Rejection::BadValue);
    // Would be valid if narrowed to 32 bits.
    CHECK(rejection_of(slot_of(SetBusMap, 0, 0x1'0000'0000, 2)) == Rejection::BadValue);
}

TEST_CASE("decode: the first fault is reported", "[command]") {
    // Unknown type, bad target, padding, and value all at once.
    auto slot = slot_of(0x101, 9, 2, 1);
    CHECK(rejection_of(slot) == Rejection::UnknownType);
    slot.type = static_cast<std::uint32_t>(CommandType::SetPlaying);
    CHECK(rejection_of(slot) == Rejection::BadTarget);
    slot.target = 0;
    CHECK(rejection_of(slot) == Rejection::NonzeroPadding);
    slot.payload[1] = 0;
    CHECK(rejection_of(slot) == Rejection::BadValue);
    slot.payload[0] = 1;
    CHECK(decode(slot).has_value());
}

} // namespace
