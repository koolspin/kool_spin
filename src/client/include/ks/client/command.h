// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Building the commands a client sends through its ring
/// (specs/ks-play/spec.md requirement 15).
///
/// Each function builds a command from absolute values, as ADR 0003 §4 asks:
/// "rate 1.02", never "rate up". None of them checks its arguments beyond
/// their types: the engine decides what is valid (specs/commands/spec.md
/// requirement 4), and Session::wait_applied() reports its answer. Reals are
/// stored as a double's IEEE-754 bit pattern in payload[0]; whole numbers as
/// they are.

#include <ks/ipc/command.h>

#include <array>
#include <cstdint>

namespace ks::client {

/// A command without its sequence number: Session::send() adds that.
struct Command {
    ipc::CommandType type = ipc::CommandType::SetPlaying;
    std::uint32_t target = 0;
    std::array<std::uint64_t, 6> payload{};

    friend bool operator==(const Command&, const Command&) = default;
};

/// Deck commands: the target is the deck's index.
[[nodiscard]] Command set_playing(std::uint32_t deck, bool on) noexcept;
[[nodiscard]] Command set_rate(std::uint32_t deck, double rate) noexcept;
[[nodiscard]] Command seek(std::uint32_t deck, double frame) noexcept;
[[nodiscard]] Command set_cue_point(std::uint32_t deck, double frame) noexcept;

/// Channel commands: the target is the channel's index (channel i is deck i).
[[nodiscard]] Command set_trim(std::uint32_t channel, double db) noexcept;
[[nodiscard]] Command set_fader(std::uint32_t channel, double position) noexcept;
/// `side`: 0 is A, 1 is B, 2 is thru. Any other value is sent as it is, for
/// the engine to reject.
[[nodiscard]] Command set_crossfader_assign(std::uint32_t channel, std::uint32_t side) noexcept;
[[nodiscard]] Command set_channel_cue(std::uint32_t channel, bool on) noexcept;

/// Mixer commands: the target is 0.
[[nodiscard]] Command set_crossfader(double position) noexcept;
[[nodiscard]] Command set_crossfader_curve(double transition) noexcept;
[[nodiscard]] Command set_main_level(double db) noexcept;
[[nodiscard]] Command set_headphone_level(double db) noexcept;
[[nodiscard]] Command set_cue_mix(double position) noexcept;
/// The main bus's first device channel, and the cue bus's.
[[nodiscard]] Command set_bus_map(std::uint32_t main, std::uint32_t cue) noexcept;

} // namespace ks::client
