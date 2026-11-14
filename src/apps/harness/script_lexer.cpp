// SPDX-License-Identifier: GPL-3.0-or-later
#include "script_lexer.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ks::harness::lexer {

namespace {

constexpr bool is_blank(char c) noexcept {
    return c == ' ' || c == '\t';
}

constexpr bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

constexpr bool is_letter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/// Whether `text` is one or more digits.
constexpr bool all_digits(std::string_view text) noexcept {
    return !text.empty() && std::ranges::all_of(text, is_digit);
}

/// Whether `text` is digits, then optionally `.` and digits.
constexpr bool is_amount(std::string_view text) noexcept {
    const std::size_t point = text.find('.');
    if (point == std::string_view::npos) {
        return all_digits(text);
    }
    return all_digits(text.substr(0, point)) && all_digits(text.substr(point + 1));
}

/// Converts text that is_amount() accepted. std::strtod, because the macOS
/// libc++ has no floating-point std::from_chars.
std::optional<double> to_double(std::string_view text) {
    // strtod follows the locale's decimal point, so confirm it consumed the
    // whole text rather than assume it.
    const std::string copy{text};
    char* end = nullptr; // NOLINT(misc-const-correctness): std::strtod's out-parameter is char**
    const double value = std::strtod(copy.c_str(), &end);
    if (end != std::to_address(copy.end()) || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::expected<std::vector<Token>, std::string> tokenize(std::string_view line) {
    std::vector<Token> tokens;
    std::size_t at = 0;
    while (at < line.size()) {
        const char c = line.at(at);
        if (is_blank(c)) {
            ++at;
            continue;
        }
        if (c == '#') {
            break;
        }
        if (c == '"') {
            const std::size_t close = line.find('"', at + 1);
            if (close == std::string_view::npos) {
                return std::unexpected{std::string{"a path has no closing quote"}};
            }
            if (close == at + 1) {
                return std::unexpected{std::string{"a path is empty"}};
            }
            const std::size_t after = close + 1;
            if (after < line.size() && !is_blank(line.at(after)) && line.at(after) != '#') {
                return std::unexpected{
                    std::string{"a closing quote must be followed by a space or a tab"}};
            }
            tokens.push_back(Token{.text = line.substr(at + 1, close - at - 1), .quoted = true});
            at = after;
            continue;
        }
        const std::size_t start = at;
        while (at < line.size() && !is_blank(line.at(at)) && line.at(at) != '#') {
            if (line.at(at) == '"') {
                return std::unexpected{std::string{"a quote must start a word"}};
            }
            ++at;
        }
        tokens.push_back(Token{.text = line.substr(start, at - start)});
    }
    return tokens;
}

std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t stop = newline == std::string_view::npos ? text.size() : newline;
        std::string_view line = text.substr(start, stop - start);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }
    return lines;
}

std::optional<std::uint64_t> parse_whole(std::string_view text, std::uint64_t max) {
    if (!all_digits(text)) {
        return std::nullopt;
    }
    const char* const first = std::to_address(text.begin());
    const char* const last = std::to_address(text.end());
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(first, last, value);
    if (error != std::errc{} || end != last || value > max) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> parse_amount(std::string_view text) {
    if (!is_amount(text)) {
        return std::nullopt;
    }
    return to_double(text);
}

std::optional<double> parse_value(std::string_view text) {
    if (text.starts_with('-')) {
        const std::optional<double> amount = parse_amount(text.substr(1));
        if (!amount) {
            return std::nullopt;
        }
        return -*amount;
    }
    return parse_amount(text);
}

std::optional<double> parse_db(std::string_view text) {
    if (text == "-inf") {
        return -std::numeric_limits<double>::infinity();
    }
    return parse_value(text);
}

bool is_track_name(std::string_view text) {
    const auto starts_name = [](char c) { return is_letter(c) || c == '_'; };
    const auto in_name = [](char c) { return is_letter(c) || is_digit(c) || c == '_' || c == '-'; };
    return !text.empty() && starts_name(text.front()) && std::ranges::all_of(text, in_name);
}

} // namespace ks::harness::lexer
