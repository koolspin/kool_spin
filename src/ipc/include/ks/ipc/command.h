// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The command slot: what clients write into their command rings (ADR 0003
/// §4, specs/commands/spec.md requirements 1 and 2).
///
/// A slot is one cache line: the client's sequence number, the command type,
/// a target, and a fixed-size payload. There's no variable-length framing, so
/// the engine decodes each slot on its own, as a pure function of its bytes.

#include <ks/ipc/spsc_ring.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ks::ipc {

/// What a command does. The values are part of the layout: never renumber
/// them. No type is 0, so a zero-filled slot is invalid. A slot keeps the type
/// as a std::uint32_t.
///
/// "Deck" and "channel" commands take a deck index as their target (channel i
/// is deck i); the others take target 0. Real values are a double's IEEE-754
/// bit pattern in payload[0]. Unused payload words are 0.
enum class CommandType : std::uint8_t {
    SetPlaying = 1,          ///< Deck. payload[0]: 0 pauses, 1 plays.
    SetRate = 2,             ///< Deck. payload[0]: the rate.
    Seek = 3,                ///< Deck. payload[0]: the track frame.
    SetCuePoint = 4,         ///< Deck. payload[0]: the track frame.
    SetTrim = 5,             ///< Channel. payload[0]: dB; −∞ allowed.
    SetFader = 6,            ///< Channel. payload[0]: the position.
    SetCrossfaderAssign = 7, ///< Channel. payload[0]: 0 is A, 1 is B, 2 is thru.
    SetChannelCue = 8,       ///< Channel. payload[0]: 0 is off, 1 is on.
    SetCrossfader = 9,       ///< payload[0]: the position.
    SetCrossfaderCurve = 10, ///< payload[0]: the transition.
    SetMainLevel = 11,       ///< payload[0]: dB; −∞ allowed.
    SetHeadphoneLevel = 12,  ///< payload[0]: dB; −∞ allowed.
    SetCueMix = 13,          ///< payload[0]: the position.
    SetBusMap = 14, ///< payload[0]: the main bus's first channel; payload[1]: the cue bus's.
};

/// One command, as a client writes it. Shared memory: integers only, and no
/// constructor, so zero-filled memory is a (rejected) slot.
struct CommandSlot {
    /// Chosen by the client. The engine reports the last one it read.
    std::uint64_t sequence;
    /// A CommandType, kept as an integer: another process wrote it.
    std::uint32_t type;
    /// The deck or channel, or 0.
    std::uint32_t target;
    /// The arguments.
    std::array<std::uint64_t, 6> payload;
};

static_assert(sizeof(CommandSlot) == 64, "a command slot is one 64-byte cache line");
static_assert(std::is_standard_layout_v<CommandSlot> && std::is_trivially_copyable_v<CommandSlot>);

/// The number of slots in a client's ring.
inline constexpr std::size_t kCommandRingCapacity = 256;

/// One client's commands to the engine.
using CommandRing = SpscRing<CommandSlot, kCommandRingCapacity>;

} // namespace ks::ipc
