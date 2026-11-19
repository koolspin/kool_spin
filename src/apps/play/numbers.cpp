// SPDX-License-Identifier: GPL-3.0-or-later

#include "numbers.h"

#include <ks/ipc/state.h>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace ks::play_app {
namespace {

bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

} // namespace

std::optional<std::uint64_t> parse_whole(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    for (const char c : text) {
        if (!is_digit(c)) {
            return std::nullopt;
        }
    }
    std::uint64_t value = 0;
    const char* const last = std::to_address(text.end());
    const auto [end, error] = std::from_chars(std::to_address(text.begin()), last, value);
    if (error != std::errc{} || end != last) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint32_t> parse_index(std::string_view text) noexcept {
    const std::optional<std::uint64_t> value = parse_whole(text);
    if (!value || *value >= ipc::kStateDecks) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*value);
}

bool is_decimal(std::string_view text) noexcept {
    std::size_t i = 0;
    const auto at = [&](std::size_t index) { return index < text.size() ? text[index] : '\0'; };
    const auto skip_digits = [&] {
        std::size_t count = 0;
        while (is_digit(at(i))) {
            ++i;
            ++count;
        }
        return count;
    };
    if (at(i) == '+' || at(i) == '-') {
        ++i;
    }
    std::size_t mantissa_digits = skip_digits();
    if (at(i) == '.') {
        ++i;
        mantissa_digits += skip_digits();
    }
    if (mantissa_digits == 0) {
        return false;
    }
    if (at(i) == 'e' || at(i) == 'E') {
        ++i;
        if (at(i) == '+' || at(i) == '-') {
            ++i;
        }
        if (skip_digits() == 0) {
            return false;
        }
    }
    return i == text.size();
}

std::optional<double> parse_real(std::string_view text, NegativeInfinity negative_infinity) {
    if (text == "-inf") {
        if (negative_infinity == NegativeInfinity::Allowed) {
            return -std::numeric_limits<double>::infinity();
        }
        return std::nullopt;
    }
    if (!is_decimal(text)) {
        return std::nullopt;
    }
    // Floating-point std::from_chars is missing from macOS's libc++, so the
    // syntax is checked above and the conversion is left to std::strtod,
    // which follows the locale's decimal point: confirm it took everything.
    const std::string terminated{text};
    char* end = nullptr; // NOLINT(misc-const-correctness): std::strtod's out-parameter is char**
    const double value = std::strtod(terminated.c_str(), &end);
    if (end != std::to_address(terminated.end()) || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

} // namespace ks::play_app
