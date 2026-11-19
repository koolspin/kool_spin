// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/client/command.h>

#include <ks/client/snapshot.h>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <limits>

namespace {

using ks::client::Command;
using ks::ipc::CommandType;

Command expected(CommandType type, std::uint32_t target, std::uint64_t first,
                 std::uint64_t second = 0) {
    Command command;
    command.type = type;
    command.target = target;
    command.payload = {first, second, 0, 0, 0, 0};
    return command;
}

std::uint64_t bits(double value) {
    return std::bit_cast<std::uint64_t>(value);
}

} // namespace

TEST_CASE("Command builders fill the slot", "[client][command]") {
    using namespace ks::client;

    CHECK(set_playing(2, true) == expected(CommandType::SetPlaying, 2, 1));
    CHECK(set_playing(3, false) == expected(CommandType::SetPlaying, 3, 0));
    CHECK(set_rate(1, 1.02) == expected(CommandType::SetRate, 1, bits(1.02)));
    CHECK(seek(0, 44100.5) == expected(CommandType::Seek, 0, bits(44100.5)));
    CHECK(set_cue_point(3, 12.0) == expected(CommandType::SetCuePoint, 3, bits(12.0)));
    CHECK(set_trim(2, -6.0) == expected(CommandType::SetTrim, 2, bits(-6.0)));
    CHECK(set_fader(1, 0.75) == expected(CommandType::SetFader, 1, bits(0.75)));
    CHECK(set_crossfader_assign(0, 2) == expected(CommandType::SetCrossfaderAssign, 0, 2));
    CHECK(set_crossfader_assign(1, 7) == expected(CommandType::SetCrossfaderAssign, 1, 7));
    CHECK(set_channel_cue(3, true) == expected(CommandType::SetChannelCue, 3, 1));
    CHECK(set_channel_cue(3, false) == expected(CommandType::SetChannelCue, 3, 0));
    CHECK(set_crossfader(-0.5) == expected(CommandType::SetCrossfader, 0, bits(-0.5)));
    CHECK(set_crossfader_curve(0.25) == expected(CommandType::SetCrossfaderCurve, 0, bits(0.25)));
    CHECK(set_main_level(-3.0) == expected(CommandType::SetMainLevel, 0, bits(-3.0)));
    CHECK(set_headphone_level(1.5) == expected(CommandType::SetHeadphoneLevel, 0, bits(1.5)));
    CHECK(set_cue_mix(0.5) == expected(CommandType::SetCueMix, 0, bits(0.5)));
    CHECK(set_bus_map(2, 4) == expected(CommandType::SetBusMap, 0, 2, 4));
}

TEST_CASE("Command builders keep NaN and infinities bit for bit", "[client][command]") {
    using namespace ks::client;
    constexpr double kInf = std::numeric_limits<double>::infinity();
    const auto nan = std::bit_cast<double>(std::uint64_t{0x7ff8'0000'dead'beefU});

    CHECK(set_rate(0, nan).payload[0] == 0x7ff8'0000'dead'beefU);
    CHECK(set_trim(1, -kInf).payload[0] == bits(-kInf));
    CHECK(set_main_level(-kInf).payload[0] == bits(-kInf));
    CHECK(set_headphone_level(kInf).payload[0] == bits(kInf));
    CHECK(seek(2, nan).payload[0] == 0x7ff8'0000'dead'beefU);
}

TEST_CASE("Rejection names", "[client][command]") {
    using ks::client::Rejection;
    using ks::client::rejection_name;

    CHECK(rejection_name(Rejection::None) == "none");
    CHECK(rejection_name(Rejection::UnknownType) == "unknown type");
    CHECK(rejection_name(Rejection::BadTarget) == "bad target");
    CHECK(rejection_name(Rejection::NonzeroPadding) == "nonzero padding");
    CHECK(rejection_name(Rejection::BadValue) == "bad value");
}
