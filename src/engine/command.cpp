// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/command.h>

#include <ks/engine/block.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ks::engine {

namespace {

using ipc::CommandType;

enum class Scope : std::uint8_t { Deck, Channel, Global };

constexpr std::uint32_t kFirstType = 1;
constexpr std::uint32_t kLastType = 14;

constexpr Scope scope_of(CommandType type) noexcept KS_NONBLOCKING {
    switch (type) {
    case CommandType::SetPlaying:
    case CommandType::SetRate:
    case CommandType::Seek:
    case CommandType::SetCuePoint:
        return Scope::Deck;
    case CommandType::SetTrim:
    case CommandType::SetFader:
    case CommandType::SetCrossfaderAssign:
    case CommandType::SetChannelCue:
        return Scope::Channel;
    case CommandType::SetCrossfader:
    case CommandType::SetCrossfaderCurve:
    case CommandType::SetMainLevel:
    case CommandType::SetHeadphoneLevel:
    case CommandType::SetCueMix:
    case CommandType::SetBusMap:
        return Scope::Global;
    }
    return Scope::Global;
}

// IEEE-754 binary64 bit patterns, checked as integers so the result doesn't
// depend on floating-point modes.
constexpr std::uint64_t kExponentMask = 0x7FF0'0000'0000'0000;
constexpr std::uint64_t kMantissaMask = 0x000F'FFFF'FFFF'FFFF;
constexpr std::uint64_t kPlusInfinity = 0x7FF0'0000'0000'0000;
constexpr std::uint64_t kMinusInfinity = 0xFFF0'0000'0000'0000;

constexpr bool is_nan(std::uint64_t bits) noexcept KS_NONBLOCKING {
    return (bits & kExponentMask) == kExponentMask && (bits & kMantissaMask) != 0;
}

constexpr bool allows_minus_infinity(CommandType type) noexcept KS_NONBLOCKING {
    return type == CommandType::SetTrim || type == CommandType::SetMainLevel ||
           type == CommandType::SetHeadphoneLevel;
}

constexpr bool valid_double(CommandType type, std::uint64_t bits) noexcept KS_NONBLOCKING {
    if (is_nan(bits) || bits == kPlusInfinity) {
        return false;
    }
    return bits != kMinusInfinity || allows_minus_infinity(type);
}

constexpr bool valid_bus_channel(std::uint64_t channel) noexcept KS_NONBLOCKING {
    return channel % 2 == 0 && channel < kMaxOutputChannels;
}

} // namespace

std::expected<Command, Rejection> decode(const ipc::CommandSlot& slot) noexcept KS_NONBLOCKING {
    // Check all 32 bits before narrowing: CommandType is only 8 bits wide.
    const std::uint32_t raw_type = slot.type;
    if (raw_type < kFirstType || raw_type > kLastType) {
        return std::unexpected{Rejection::UnknownType};
    }
    const auto type = static_cast<CommandType>(raw_type);
    const Scope scope = scope_of(type);

    const std::uint32_t target = slot.target;
    if (scope == Scope::Global ? target != 0 : target >= kDecks) {
        return std::unexpected{Rejection::BadTarget};
    }

    const std::size_t first_padding = type == CommandType::SetBusMap ? 2 : 1;
    for (const std::uint64_t word : std::span{slot.payload}.subspan(first_padding)) {
        if (word != 0) {
            return std::unexpected{Rejection::NonzeroPadding};
        }
    }

    Command command;
    command.type = type;
    if (scope != Scope::Global) {
        command.deck = target;
    }

    const std::uint64_t argument = slot.payload[0];
    switch (type) {
    case CommandType::SetPlaying:
    case CommandType::SetChannelCue:
        if (argument > 1) {
            return std::unexpected{Rejection::BadValue};
        }
        command.on = argument == 1;
        break;
    case CommandType::SetCrossfaderAssign:
        switch (argument) {
        case 0:
            command.side = CrossfaderSide::A;
            break;
        case 1:
            command.side = CrossfaderSide::B;
            break;
        case 2:
            command.side = CrossfaderSide::Thru;
            break;
        default:
            return std::unexpected{Rejection::BadValue};
        }
        break;
    case CommandType::SetBusMap: {
        // Compare the 64-bit words before narrowing them.
        const std::uint64_t main = argument;
        const std::uint64_t cue = slot.payload[1];
        if (!valid_bus_channel(main) || !valid_bus_channel(cue) || main == cue) {
            return std::unexpected{Rejection::BadValue};
        }
        command.bus_map =
            BusMap{.main = static_cast<std::size_t>(main), .cue = static_cast<std::size_t>(cue)};
        break;
    }
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
        if (!valid_double(type, argument)) {
            return std::unexpected{Rejection::BadValue};
        }
        command.value = std::bit_cast<double>(argument);
        break;
    }
    return command;
}

void apply(const Command& command, Mixer& mixer) noexcept KS_NONBLOCKING {
    // decode() only builds deck indices below kDecks, but a Command can be
    // built by hand: the mixer's controls ignore a bad index, and deck()
    // requires a good one.
    const std::size_t deck = command.deck;
    switch (command.type) {
    case CommandType::SetPlaying:
        if (deck < kDecks) {
            mixer.deck(deck).set_playing(command.on);
        }
        break;
    case CommandType::SetRate:
        if (deck < kDecks) {
            mixer.deck(deck).set_rate(command.value);
        }
        break;
    case CommandType::Seek:
        if (deck < kDecks) {
            mixer.deck(deck).seek(command.value);
        }
        break;
    case CommandType::SetCuePoint:
        if (deck < kDecks) {
            mixer.deck(deck).set_cue(command.value);
        }
        break;
    case CommandType::SetTrim:
        mixer.set_trim(deck, command.value);
        break;
    case CommandType::SetFader:
        mixer.set_fader(deck, command.value);
        break;
    case CommandType::SetCrossfaderAssign:
        mixer.set_crossfader_assign(deck, command.side);
        break;
    case CommandType::SetChannelCue:
        mixer.set_cue(deck, command.on);
        break;
    case CommandType::SetCrossfader:
        mixer.set_crossfader(command.value);
        break;
    case CommandType::SetCrossfaderCurve:
        mixer.set_crossfader_curve(command.value);
        break;
    case CommandType::SetMainLevel:
        mixer.set_main_level(command.value);
        break;
    case CommandType::SetHeadphoneLevel:
        mixer.set_headphone_level(command.value);
        break;
    case CommandType::SetCueMix:
        mixer.set_cue_mix(command.value);
        break;
    case CommandType::SetBusMap:
        mixer.set_bus_map(command.bus_map);
        break;
    }
}

} // namespace ks::engine
