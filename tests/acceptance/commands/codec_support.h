// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the decoder tests and the decoder fuzz target: an encoder written
// from specs/commands/spec.md requirement 2, independently of decode(); a
// generator of valid commands; and requirement 15's round-trip properties.

#include <ks/engine/block.h>
#include <ks/engine/command.h>
#include <ks/engine/mixer.h>
#include <ks/ipc/command.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace ks::engine::test {

/// A double's IEEE-754 bit pattern, as a slot carries it.
inline std::uint64_t bits_of(double value) {
    return std::bit_cast<std::uint64_t>(value);
}

/// Whether `type` takes a deck or channel as its target.
inline bool takes_deck(ipc::CommandType type) {
    using enum ipc::CommandType;
    switch (type) {
    case SetPlaying:
    case SetRate:
    case Seek:
    case SetCuePoint:
    case SetTrim:
    case SetFader:
    case SetCrossfaderAssign:
    case SetChannelCue:
        return true;
    case SetCrossfader:
    case SetCrossfaderCurve:
    case SetMainLevel:
    case SetHeadphoneLevel:
    case SetCueMix:
    case SetBusMap:
        return false;
    }
    return false;
}

/// Whether `type` carries a double in payload[0].
inline bool takes_double(ipc::CommandType type) {
    using enum ipc::CommandType;
    switch (type) {
    case SetRate:
    case Seek:
    case SetCuePoint:
    case SetTrim:
    case SetFader:
    case SetCrossfader:
    case SetCrossfaderCurve:
    case SetMainLevel:
    case SetHeadphoneLevel:
    case SetCueMix:
        return true;
    case SetPlaying:
    case SetCrossfaderAssign:
    case SetChannelCue:
    case SetBusMap:
        return false;
    }
    return false;
}

/// Whether `type`'s double may be −∞ (requirement 4).
inline bool allows_minus_infinity(ipc::CommandType type) {
    using enum ipc::CommandType;
    return type == SetTrim || type == SetMainLevel || type == SetHeadphoneLevel;
}

/// A side's code in payload[0]: 0 is A, 1 is B, 2 is thru.
inline std::uint64_t side_code(CrossfaderSide side) {
    switch (side) {
    case CrossfaderSide::A:
        return 0;
    case CrossfaderSide::B:
        return 1;
    case CrossfaderSide::Thru:
        return 2;
    }
    return 2;
}

/// Encodes `command` with `sequence`, as requirement 2 says. Arguments the
/// type doesn't use are left out, so their words stay 0.
inline ipc::CommandSlot encode(std::uint64_t sequence, const Command& command) {
    ipc::CommandSlot slot{};
    slot.sequence = sequence;
    slot.type = static_cast<std::uint32_t>(command.type);
    if (takes_deck(command.type)) {
        slot.target = static_cast<std::uint32_t>(command.deck);
    }
    using enum ipc::CommandType;
    switch (command.type) {
    case SetPlaying:
    case SetChannelCue:
        slot.payload[0] = command.on ? 1 : 0;
        break;
    case SetCrossfaderAssign:
        slot.payload[0] = side_code(command.side);
        break;
    case SetBusMap:
        slot.payload[0] = command.bus_map.main;
        slot.payload[1] = command.bus_map.cue;
        break;
    case SetRate:
    case Seek:
    case SetCuePoint:
    case SetTrim:
    case SetFader:
    case SetCrossfader:
    case SetCrossfaderCurve:
    case SetMainLevel:
    case SetHeadphoneLevel:
    case SetCueMix:
        slot.payload[0] = bits_of(command.value);
        break;
    }
    return slot;
}

/// Whether two slots hold the same 64 bytes.
inline bool same_bytes(const ipc::CommandSlot& a, const ipc::CommandSlot& b) {
    return a.sequence == b.sequence && a.type == b.type && a.target == b.target &&
           a.payload == b.payload;
}

/// Reads bytes in order, and zeros once they run out.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> bytes) : bytes_{bytes} {}

    std::uint8_t byte() {
        if (next_ >= bytes_.size()) {
            return 0;
        }
        return bytes_[next_++];
    }

    std::uint64_t word() {
        std::uint64_t word = 0;
        for (int i = 0; i < 8; ++i) {
            word = (word << 8U) | byte();
        }
        return word;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t next_ = 0;
};

/// A valid command chosen by the bytes `in` reads: every type, deck, side,
/// flag, and valid bus map, and doubles from arbitrary bit patterns (special
/// values included), made valid where they aren't.
inline Command valid_command(ByteReader& in) {
    Command command;
    command.type = static_cast<ipc::CommandType>(1 + (in.byte() % 14));
    const std::size_t deck = in.byte() % kDecks;
    const std::uint64_t raw = in.word();
    const std::uint8_t pick = in.byte();
    if (takes_deck(command.type)) {
        command.deck = deck;
    }
    using enum ipc::CommandType;
    switch (command.type) {
    case SetPlaying:
    case SetChannelCue:
        command.on = (pick & 1U) != 0;
        break;
    case SetCrossfaderAssign: {
        const std::uint8_t code = pick % 3;
        if (code == 0) {
            command.side = CrossfaderSide::A;
        } else if (code == 1) {
            command.side = CrossfaderSide::B;
        } else {
            command.side = CrossfaderSide::Thru;
        }
        break;
    }
    case SetBusMap: {
        // Distinct even channels below kMaxOutputChannels.
        constexpr std::size_t kPairs = kMaxOutputChannels / 2;
        const std::size_t main = raw % kPairs;
        const std::size_t cue = (main + 1 + ((raw >> 8U) % (kPairs - 1))) % kPairs;
        command.bus_map = BusMap{.main = 2 * main, .cue = 2 * cue};
        break;
    }
    case SetRate:
    case Seek:
    case SetCuePoint:
    case SetTrim:
    case SetFader:
    case SetCrossfader:
    case SetCrossfaderCurve:
    case SetMainLevel:
    case SetHeadphoneLevel:
    case SetCueMix: {
        const auto value = std::bit_cast<double>(raw);
        if (std::isfinite(value)) {
            command.value = value;
        } else if (allows_minus_infinity(command.type) && (pick & 1U) != 0) {
            command.value = -std::numeric_limits<double>::infinity();
        } else {
            // Clearing the exponent's top bit always leaves a finite value.
            command.value = std::bit_cast<double>(raw & ~(std::uint64_t{1} << 62U));
        }
        break;
    }
    }
    return command;
}

/// Requirement 15, bytes to command: if `slot` decodes, encoding the result
/// with the same sequence number gives back the same 64 bytes.
inline bool slot_round_trips(const ipc::CommandSlot& slot) {
    const auto decoded = decode(slot);
    if (!decoded.has_value()) {
        return true;
    }
    return same_bytes(encode(slot.sequence, *decoded), slot);
}

/// Requirement 15, command to bytes: encoding a valid command and decoding
/// the result gives back the same command, with its value bit for bit.
inline bool command_round_trips(std::uint64_t sequence, const Command& command) {
    const auto decoded = decode(encode(sequence, command));
    return decoded.has_value() && *decoded == command &&
           bits_of(decoded->value) == bits_of(command.value);
}

} // namespace ks::engine::test
