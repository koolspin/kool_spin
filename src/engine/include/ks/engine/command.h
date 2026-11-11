// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Turning a command slot into a validated command, and applying it to the
/// mixer and its decks (specs/commands/spec.md requirements 3 to 5).

#include <ks/core/realtime.h>
#include <ks/engine/mixer.h>
#include <ks/ipc/command.h>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace ks::engine {

/// Why decode() refused a slot. decode() checks for them in this order and
/// reports the first.
enum class Rejection : std::uint8_t {
    /// The type isn't a CommandType.
    UnknownType,
    /// A deck or channel of kDecks or more, or a nonzero target for another
    /// command.
    BadTarget,
    /// A payload word that should be 0 isn't.
    NonzeroPadding,
    /// The argument isn't allowed (spec requirement 4).
    BadValue,
};

/// A validated command. Each type uses only some of the fields; the others
/// hold their defaults.
struct Command {
    ipc::CommandType type = ipc::CommandType::SetPlaying;
    /// The deck or channel, for deck and channel commands.
    std::size_t deck = 0;
    /// The real value: a rate, a track frame, dB, a position, or a transition.
    double value = 0.0;
    /// SetPlaying and SetChannelCue.
    bool on = false;
    /// SetCrossfaderAssign.
    CrossfaderSide side = CrossfaderSide::Thru;
    /// SetBusMap.
    BusMap bus_map{};

    bool operator==(const Command&) const = default;
};

/// Decodes and validates `slot`. A pure function of the slot's bytes: any 64
/// bytes are safe. The slot's sequence number isn't validated.
[[nodiscard]] std::expected<Command, Rejection>
decode(const ipc::CommandSlot& slot) noexcept KS_NONBLOCKING;

/// Calls the deck or mixer control that `command` names, with its arguments.
void apply(const Command& command, Mixer& mixer) noexcept KS_NONBLOCKING;

} // namespace ks::engine
