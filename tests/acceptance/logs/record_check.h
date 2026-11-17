// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The checks the log record fuzz target makes (specs/logs/spec.md,
// "Fuzzing"), which "Log lines: format" also runs, and the UTF-8 checks the
// other tests use. Standard library and ks::ipc only, so the fuzz target can
// include it.

#include <ks/ipc/log.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace ks::logs::test {

/// True if `text` is valid UTF-8: no stray continuation bytes, no truncated
/// or overlong sequences, no surrogates, nothing above U+10FFFF.
inline bool valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto byte = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        std::uint32_t point = 0;
        if (byte < 0x80U) {
            ++i;
            continue;
        }
        if (byte >= 0xC2U && byte <= 0xDFU) {
            length = 2;
            point = byte & 0x1FU;
        } else if (byte >= 0xE0U && byte <= 0xEFU) {
            length = 3;
            point = byte & 0x0FU;
        } else if (byte >= 0xF0U && byte <= 0xF4U) {
            length = 4;
            point = byte & 0x07U;
        } else {
            return false;
        }
        if (i + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            point = (point << 6U) | (next & 0x3FU);
        }
        const bool overlong = (length == 3 && point < 0x800U) || (length == 4 && point < 0x10000U);
        if (overlong || (point >= 0xD800U && point <= 0xDFFFU) || point > 0x10FFFFU) {
            return false;
        }
        i += length;
    }
    return true;
}

/// True if `text` is valid UTF-8 with no control character: U+0000 to
/// U+001F, U+007F, or U+0080 to U+009F (spec requirement 6).
inline bool clean(std::string_view text) {
    if (!valid_utf8(text)) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x20U || byte == 0x7FU) {
            return false;
        }
        // U+0080 to U+009F are C2 80 to C2 9F.
        if (byte == 0xC2U && i + 1 < text.size()) {
            const auto next = static_cast<unsigned char>(text[i + 1]);
            if (next >= 0x80U && next <= 0x9FU) {
                return false;
            }
        }
    }
    return true;
}

/// True if `line` starts with a shown time ("YYYY-MM-DD HH:MM:SS.ffffff") or
/// "steady:<digits>", then a level, then a space.
inline bool line_shape_ok(std::string_view line) {
    std::size_t rest = 0;
    constexpr std::string_view kShape = "dddd-dd-dd dd:dd:dd.dddddd";
    if (line.starts_with("steady:")) {
        rest = 7;
        const std::size_t digits_start = rest;
        while (rest < line.size() && line[rest] >= '0' && line[rest] <= '9') {
            ++rest;
        }
        if (rest == digits_start) {
            return false;
        }
    } else {
        if (line.size() < kShape.size()) {
            return false;
        }
        for (std::size_t i = 0; i < kShape.size(); ++i) {
            const bool digit = line[i] >= '0' && line[i] <= '9';
            if (kShape[i] == 'd' ? !digit : line[i] != kShape[i]) {
                return false;
            }
        }
        rest = kShape.size();
    }
    const std::string_view after = line.substr(rest);
    return after.starts_with(" error ") || after.starts_with(" warning ") ||
           after.starts_with(" info ");
}

/// Builds a LogRecord and a LogClock from `data` (zero-padded), formats the
/// record, and checks the result is one clean, line-shaped line. The fuzz
/// target calls it with every input.
inline bool check_record_input(std::span<const std::uint8_t> data) {
    std::array<std::uint8_t, sizeof(ipc::LogRecord) + 24> bytes{};
    std::ranges::copy(data.first(std::min(data.size(), bytes.size())), bytes.begin());
    ipc::LogRecord record{};
    std::memcpy(&record, bytes.data(), sizeof record);
    ipc::LogClock clock{};
    const std::span<const std::uint8_t> tail = std::span{bytes}.subspan(sizeof record);
    std::memcpy(&clock.steady_ns, tail.data(), 8);
    std::memcpy(&clock.system_ns, tail.subspan(8).data(), 8);
    std::memcpy(&clock.utc_offset_s, tail.subspan(16).data(), 8);
    const std::string line = ipc::format_log_record(clock, record);
    return !line.empty() && clean(line) && line_shape_ok(line);
}

} // namespace ks::logs::test
