// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The script reader's lexer (specs/harness/spec.md requirements 4 and 5):
/// splitting a line into tokens, and the strict number syntax. Private to
/// the harness.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ks::harness::lexer {

/// One token of a line.
struct Token {
    /// The text, without the quotes for a quoted token.
    std::string_view text;
    /// Whether it was in double quotes: only a path may be.
    bool quoted = false;

    bool operator==(const Token&) const = default;
};

/// Splits `line` (without its line end) into tokens separated by spaces or
/// tabs, dropping a `#` comment that isn't inside quotes. A quoted token
/// runs from one `"` to the next. The tokens view `line`. An error, in
/// words, for an unterminated quote, an empty quoted token, or a quote that
/// doesn't start or end a token.
[[nodiscard]] std::expected<std::vector<Token>, std::string> tokenize(std::string_view line);

/// Splits `text` into lines on `\n`, dropping one `\r` before each `\n` and
/// at the very end. A final line with no `\n` counts; an empty text has no
/// lines. The lines view `text`.
[[nodiscard]] std::vector<std::string_view> split_lines(std::string_view text);

/// A whole number: digits only, at most `max`. Nothing for anything else.
[[nodiscard]] std::optional<std::uint64_t> parse_whole(std::string_view text, std::uint64_t max);

/// An amount: digits, then optionally `.` and digits. Finite. Nothing for
/// anything else.
[[nodiscard]] std::optional<double> parse_amount(std::string_view text);

/// A value: an amount with an optional leading `-`.
[[nodiscard]] std::optional<double> parse_value(std::string_view text);

/// Decibels: a value, or `-inf`.
[[nodiscard]] std::optional<double> parse_db(std::string_view text);

/// Whether `text` is a track name: a letter or `_`, then letters, digits,
/// `_`, or `-`.
[[nodiscard]] bool is_track_name(std::string_view text);

} // namespace ks::harness::lexer
