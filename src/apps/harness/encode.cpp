// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/harness/encode.h>

#include <ks/engine/command.h>
#include <ks/engine/mixer.h>
#include <ks/ipc/command.h>

#include <bit>
#include <cstdint>

namespace ks::harness {

namespace {

using ipc::CommandType;

/// Deck and channel commands target a deck; the others target 0.
constexpr bool has_target(CommandType type) noexcept {
    switch (type) {
    case CommandType::SetPlaying:
    case CommandType::SetRate:
    case CommandType::Seek:
    case CommandType::SetCuePoint:
    case CommandType::SetTrim:
    case CommandType::SetFader:
    case CommandType::SetCrossfaderAssign:
    case CommandType::SetChannelCue:
        return true;
    case CommandType::SetCrossfader:
    case CommandType::SetCrossfaderCurve:
    case CommandType::SetMainLevel:
    case CommandType::SetHeadphoneLevel:
    case CommandType::SetCueMix:
    case CommandType::SetBusMap:
        return false;
    }
    return false;
}

/// payload[0] for a crossfader assignment: 0 is A, 1 is B, 2 is thru.
constexpr std::uint64_t side_word(engine::CrossfaderSide side) noexcept {
    switch (side) {
    case engine::CrossfaderSide::A:
        return 0;
    case engine::CrossfaderSide::B:
        return 1;
    case engine::CrossfaderSide::Thru:
        return 2;
    }
    return 2;
}

} // namespace

ipc::CommandSlot encode(const engine::Command& command, std::uint64_t sequence) noexcept {
    ipc::CommandSlot slot{};
    slot.sequence = sequence;
    slot.type = static_cast<std::uint32_t>(command.type);
    slot.target = has_target(command.type) ? static_cast<std::uint32_t>(command.deck) : 0;

    switch (command.type) {
    case CommandType::SetPlaying:
    case CommandType::SetChannelCue:
        slot.payload[0] = command.on ? 1 : 0;
        break;
    case CommandType::SetCrossfaderAssign:
        slot.payload[0] = side_word(command.side);
        break;
    case CommandType::SetBusMap:
        slot.payload[0] = command.bus_map.main;
        slot.payload[1] = command.bus_map.cue;
        break;
    case CommandType::SetRate:
    case CommandType::Seek:
    case CommandType::SetCuePoint:
    case CommandType::SetTrim:
    case CommandType::SetFader:
    case CommandType::SetCrossfader:
    case CommandType::SetCrossfaderCurve:
    case CommandType::SetMainLevel:
    case CommandType::SetHeadphoneLevel:
    case CommandType::SetCueMix:
        slot.payload[0] = std::bit_cast<std::uint64_t>(command.value);
        break;
    }
    return slot;
}

} // namespace ks::harness
