// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/tone/options.h>

#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ks::tone {
namespace {

bool is_digit(char c) noexcept {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

/// Accepts only an optional '-', then digits with at most one '.', with at
/// least one digit in total. This rejects everything std::strtod would also
/// take but the spec doesn't allow: "nan", "inf", exponents, hex, '+', spaces.
bool is_plain_decimal(std::string_view text) noexcept {
    if (text.starts_with('-')) {
        text.remove_prefix(1);
    }
    bool seen_digit = false;
    bool seen_point = false;
    for (const char c : text) {
        if (is_digit(c)) {
            seen_digit = true;
        } else if (c == '.' && !seen_point) {
            seen_point = true;
        } else {
            return false;
        }
    }
    return seen_digit;
}

/// Parses the value of `option`: a plain decimal number within [min, max].
std::expected<double, std::string> parse_number(std::string_view option, std::string_view text,
                                                double min, double max) {
    const auto error = [&] {
        return std::unexpected{
            std::format("ks-tone: {} must be a number from {} to {}", option, min, max)};
    };
    if (!is_plain_decimal(text)) {
        return error();
    }
    // Floating-point std::from_chars is missing from macOS's libc++, so the
    // syntax is checked above and the conversion is left to std::strtod. Every
    // string that passes the check is one strtod consumes completely.
    const std::string terminated{text};
    const double value = std::strtod(terminated.c_str(), nullptr);
    if (!std::isfinite(value) || value < min || value > max) {
        return error();
    }
    return value;
}

std::optional<std::uint32_t> parse_device(std::string_view text) noexcept {
    const char* const first = std::to_address(text.begin());
    const char* const last = std::to_address(text.end());
    std::uint32_t value = 0;
    const auto [end, ec] = std::from_chars(first, last, value);
    if (text.empty() || ec != std::errc{} || end != last) {
        return std::nullopt;
    }
    return value;
}

bool takes_value(std::string_view arg) noexcept {
    return arg == "--backend" || arg == "--frequency" || arg == "--level" || arg == "--duration" ||
           arg == "--device";
}

/// Applies `value` to the option `arg`, one of those takes_value() accepts.
std::expected<void, std::string> apply_value(Options& options, std::string_view arg,
                                             std::string_view value) {
    if (arg == "--backend") {
        options.backend = value;
        return {};
    }
    if (arg == "--device") {
        const auto device = parse_device(value);
        if (!device) {
            return std::unexpected{
                "ks-tone: --device must be a device number shown by --list-devices"};
        }
        options.device = device;
        return {};
    }
    if (arg == "--frequency") {
        return parse_number(arg, value, kMinFrequencyHz, kMaxFrequencyHz).transform([&](double hz) {
            options.settings.frequency_hz = hz;
        });
    }
    if (arg == "--level") {
        return parse_number(arg, value, kMinLevelDbfs, kMaxLevelDbfs).transform([&](double dbfs) {
            options.settings.level_dbfs = dbfs;
        });
    }
    return parse_number(arg, value, kMinDurationSeconds, kMaxDurationSeconds)
        .transform(
            [&](double seconds) { options.duration = std::chrono::duration<double>{seconds}; });
}

} // namespace

std::expected<Options, std::string> parse_options(std::span<const std::string_view> args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            options.help = true;
            return options;
        }
        if (arg == "--list-devices") {
            options.list_devices = true;
            continue;
        }
        if (!takes_value(arg)) {
            return std::unexpected{std::format("ks-tone: unexpected argument '{}'", arg)};
        }
        if (i + 1 >= args.size()) {
            return std::unexpected{std::format("ks-tone: {} needs a value", arg)};
        }
        if (auto applied = apply_value(options, arg, args[++i]); !applied) {
            return std::unexpected{std::move(applied.error())};
        }
    }
    return options;
}

} // namespace ks::tone
