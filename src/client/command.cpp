// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/client/command.h>

#include <bit>

namespace ks::client {

namespace {

[[nodiscard]] Command make(ipc::CommandType type, std::uint32_t target,
                           std::uint64_t value) noexcept {
    Command command;
    command.type = type;
    command.target = target;
    command.payload[0] = value;
    return command;
}

/// A real as its IEEE-754 bit pattern, so NaN and the infinities survive
/// bit for bit for the engine to judge.
[[nodiscard]] std::uint64_t real(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] std::uint64_t flag(bool value) noexcept {
    return value ? 1 : 0;
}

} // namespace

Command set_playing(std::uint32_t deck, bool on) noexcept {
    return make(ipc::CommandType::SetPlaying, deck, flag(on));
}
Command set_rate(std::uint32_t deck, double rate) noexcept {
    return make(ipc::CommandType::SetRate, deck, real(rate));
}
Command seek(std::uint32_t deck, double frame) noexcept {
    return make(ipc::CommandType::Seek, deck, real(frame));
}
Command set_cue_point(std::uint32_t deck, double frame) noexcept {
    return make(ipc::CommandType::SetCuePoint, deck, real(frame));
}
Command set_trim(std::uint32_t channel, double db) noexcept {
    return make(ipc::CommandType::SetTrim, channel, real(db));
}
Command set_fader(std::uint32_t channel, double position) noexcept {
    return make(ipc::CommandType::SetFader, channel, real(position));
}
Command set_crossfader_assign(std::uint32_t channel, std::uint32_t side) noexcept {
    return make(ipc::CommandType::SetCrossfaderAssign, channel, side);
}
Command set_channel_cue(std::uint32_t channel, bool on) noexcept {
    return make(ipc::CommandType::SetChannelCue, channel, flag(on));
}
Command set_crossfader(double position) noexcept {
    return make(ipc::CommandType::SetCrossfader, 0, real(position));
}
Command set_crossfader_curve(double transition) noexcept {
    return make(ipc::CommandType::SetCrossfaderCurve, 0, real(transition));
}
Command set_main_level(double db) noexcept {
    return make(ipc::CommandType::SetMainLevel, 0, real(db));
}
Command set_headphone_level(double db) noexcept {
    return make(ipc::CommandType::SetHeadphoneLevel, 0, real(db));
}
Command set_cue_mix(double position) noexcept {
    return make(ipc::CommandType::SetCueMix, 0, real(position));
}
Command set_bus_map(std::uint32_t main, std::uint32_t cue) noexcept {
    Command command = make(ipc::CommandType::SetBusMap, 0, main);
    command.payload[1] = cue;
    return command;
}

} // namespace ks::client
