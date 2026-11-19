// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// ks-play's strict number parsers (specs/ks-play/spec.md requirement 22).
/// Private to ks_play_app; its unit tests include it directly.

#include <cstdint>
#include <optional>
#include <string_view>

namespace ks::play_app {

/// A whole number: digits only (no sign, no space), within std::uint64_t.
[[nodiscard]] std::optional<std::uint64_t> parse_whole(std::string_view text) noexcept;

/// A deck or a channel: a whole number from 0 to 3.
[[nodiscard]] std::optional<std::uint32_t> parse_index(std::string_view text) noexcept;

/// True if `text` is `[+-]?digits[.digits][(e|E)[+-]?digits]`, where either
/// side of the point may be empty but the mantissa has at least one digit.
[[nodiscard]] bool is_decimal(std::string_view text) noexcept;

/// Whether a command takes `-inf` (trim and the two levels do).
enum class NegativeInfinity : std::uint8_t { Refused, Allowed };

/// A real: a finite decimal (is_decimal()), or the word `-inf` where
/// allowed. A literal that overflows to an infinity is refused.
[[nodiscard]] std::optional<double> parse_real(std::string_view text,
                                               NegativeInfinity negative_infinity);

} // namespace ks::play_app
