// SPDX-License-Identifier: GPL-3.0-or-later
// The log clock and the line formats (specs/logs/spec.md requirements 5 to
// 7). Everything here formats values another process may have written: any
// record, with any clock, becomes one line of valid UTF-8 with no control
// characters.
#include <ks/ipc/log.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace ks::ipc {

namespace {

constexpr std::int64_t kNsPerSecond = 1'000'000'000;
constexpr std::int64_t kNsPerMicrosecond = 1'000;
constexpr std::int64_t kSecondsPerMinute = 60;
constexpr std::int64_t kSecondsPerHour = 3'600;
constexpr std::int64_t kSecondsPerDay = 86'400;

/// 10000-01-01 00:00:00, in seconds since 1970-01-01 00:00:00: the first
/// time that can't be shown with a four-digit year.
constexpr std::int64_t kEndOfShownTime = 253'402'300'800;

/// UTC offsets beyond these can't give a time that can be shown, whatever
/// the clocks say: a system time and a steady difference are each a 64-bit
/// count of nanoseconds, under 2^65 ns (3.7e10 s) together.
constexpr std::int64_t kOffsetLimit = 2 * kEndOfShownTime;

/// A local time: seconds since 1970-01-01 00:00:00, and nanoseconds within
/// the second.
struct LocalTime {
    std::int64_t seconds;
    std::int64_t nanos;
};

/// `time`, a steady time in nanoseconds, as `clock`'s local time, or
/// nothing if it falls outside the years 1970 to 9999. Works in seconds and
/// nanoseconds, so nothing overflows whatever the three values are.
std::optional<LocalTime> local_time(const LogClock& clock, std::uint64_t time) noexcept {
    // The system time, rounded down to a second.
    std::int64_t seconds = clock.system_ns / kNsPerSecond;
    std::int64_t nanos = clock.system_ns % kNsPerSecond;
    if (nanos < 0) {
        nanos += kNsPerSecond;
        --seconds;
    }
    // Moved by the steady time since the clock was read, or before it.
    constexpr auto kNs = static_cast<std::uint64_t>(kNsPerSecond);
    if (time >= clock.steady_ns) {
        const std::uint64_t later = time - clock.steady_ns;
        seconds += static_cast<std::int64_t>(later / kNs);
        nanos += static_cast<std::int64_t>(later % kNs);
        if (nanos >= kNsPerSecond) {
            nanos -= kNsPerSecond;
            ++seconds;
        }
    } else {
        const std::uint64_t earlier = clock.steady_ns - time;
        seconds -= static_cast<std::int64_t>(earlier / kNs);
        nanos -= static_cast<std::int64_t>(earlier % kNs);
        if (nanos < 0) {
            nanos += kNsPerSecond;
            --seconds;
        }
    }
    // `seconds` is within 3.7e10 of zero, so adding a checked offset is safe.
    if (clock.utc_offset_s < -kOffsetLimit || clock.utc_offset_s > kOffsetLimit) {
        return std::nullopt;
    }
    seconds += clock.utc_offset_s;
    if (seconds < 0 || seconds >= kEndOfShownTime) {
        return std::nullopt;
    }
    return LocalTime{.seconds = seconds, .nanos = nanos};
}

/// "YYYY-MM-DD HH:MM:SS.ffffff", rounded down to the microsecond. The date
/// is arithmetic on the day count: no time zone or locale is consulted.
std::string shown(const LocalTime& local) {
    const std::chrono::year_month_day date{
        std::chrono::sys_days{std::chrono::days{local.seconds / kSecondsPerDay}}};
    const std::int64_t in_day = local.seconds % kSecondsPerDay;
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:06}", static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                       in_day / kSecondsPerHour, (in_day % kSecondsPerHour) / kSecondsPerMinute,
                       in_day % kSecondsPerMinute, local.nanos / kNsPerMicrosecond);
}

/// A line's time: the local date and time, or "steady:<n>" if that can't be
/// shown.
std::string time_text(const LogClock& clock, std::uint64_t time) {
    if (const std::optional<LocalTime> local = local_time(clock, time)) {
        return shown(*local);
    }
    return std::format("steady:{}", time);
}

/// A level's name, or nothing for a value that isn't a level.
std::optional<std::string_view> level_name(std::uint64_t level) noexcept {
    switch (level) {
    case static_cast<std::uint64_t>(LogLevel::Error):
        return "error";
    case static_cast<std::uint64_t>(LogLevel::Warning):
        return "warning";
    case static_cast<std::uint64_t>(LogLevel::Info):
        return "info";
    default:
        return std::nullopt;
    }
}

constexpr std::uint8_t byte_at(std::string_view text, std::size_t index) noexcept {
    return static_cast<std::uint8_t>(text[index]);
}

/// The UTF-8 character at the start of `text`.
struct Character {
    std::size_t length; ///< In bytes, or 0 if the bytes aren't a valid character.
    std::uint32_t point;
};

/// Decodes the character at the start of `text`, which isn't empty. Refuses
/// stray continuation bytes, characters cut short, overlong forms,
/// surrogates, and values above U+10FFFF.
Character decode(std::string_view text) noexcept {
    constexpr Character kInvalid{.length = 0, .point = 0};
    const std::uint8_t first = byte_at(text, 0);
    if (first < 0x80U) {
        return {.length = 1, .point = first};
    }
    Character character{};
    if (first >= 0xC2U && first <= 0xDFU) {
        character = {.length = 2, .point = first & 0x1FU};
    } else if (first >= 0xE0U && first <= 0xEFU) {
        character = {.length = 3, .point = first & 0x0FU};
    } else if (first >= 0xF0U && first <= 0xF4U) {
        character = {.length = 4, .point = first & 0x07U};
    } else {
        return kInvalid;
    }
    if (text.size() < character.length) {
        return kInvalid;
    }
    for (std::size_t i = 1; i < character.length; ++i) {
        const std::uint8_t next = byte_at(text, i);
        if ((next & 0xC0U) != 0x80U) {
            return kInvalid;
        }
        character.point = (character.point << 6U) | (next & 0x3FU);
    }
    const bool overlong = (character.length == 3 && character.point < 0x800U) ||
                          (character.length == 4 && character.point < 0x1'0000U);
    const bool surrogate = character.point >= 0xD800U && character.point <= 0xDFFFU;
    if (overlong || surrogate || character.point > 0x10'FFFFU) {
        return kInvalid;
    }
    return character;
}

/// True for U+0000 to U+001F, U+007F, and U+0080 to U+009F.
constexpr bool is_control(std::uint32_t point) noexcept {
    return point < 0x20U || (point >= 0x7FU && point <= 0x9FU);
}

/// Appends `text` to `line`, with each byte of a control character, and each
/// byte that isn't part of valid UTF-8, as \xNN.
void append_escaped(std::string& line, std::string_view text) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    while (!text.empty()) {
        const Character character = decode(text);
        if (character.length > 0 && !is_control(character.point)) {
            line += text.substr(0, character.length);
            text.remove_prefix(character.length);
            continue;
        }
        // A control character is escaped whole; of invalid bytes, one at a
        // time, since the next byte may start a valid character.
        const std::size_t escaped = character.length > 0 ? character.length : 1;
        for (std::size_t i = 0; i < escaped; ++i) {
            const std::uint8_t byte = byte_at(text, i);
            line += "\\x";
            line += kDigits[byte >> 4U];
            line += kDigits[byte & 0x0FU];
        }
        text.remove_prefix(escaped);
    }
}

constexpr std::string_view kInvalidRecord = " warning [invalid log record]";
constexpr std::string_view kEllipsis = "\xE2\x80\xA6"; // U+2026

/// "<time> <level> <text>", with the text escaped, and "…" after it if it
/// was truncated. A level that isn't one makes the line an invalid record's.
std::string format_line(const LogClock& clock, std::uint64_t time, std::uint64_t level,
                        std::string_view text, bool truncated) {
    std::string line = time_text(clock, time);
    const std::optional<std::string_view> name = level_name(level);
    if (!name) {
        line += kInvalidRecord;
        return line;
    }
    line += ' ';
    line += *name;
    line += ' ';
    append_escaped(line, text);
    if (truncated) {
        line += kEllipsis;
    }
    return line;
}

} // namespace

LogClock LogClock::now() {
    const std::chrono::steady_clock::time_point steady = std::chrono::steady_clock::now();
    const std::chrono::system_clock::time_point system = std::chrono::system_clock::now();
    LogClock clock{};
    clock.steady_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(steady.time_since_epoch()).count());
    clock.system_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(system.time_since_epoch()).count();
    // The one time-zone lookup of a session. If it fails, lines show UTC.
    const std::time_t seconds = std::chrono::system_clock::to_time_t(system);
    std::tm local{};
    if (localtime_r(&seconds, &local) != nullptr) {
        clock.utc_offset_s = static_cast<std::int64_t>(local.tm_gmtoff);
    }
    return clock;
}

std::string format_log_line(const LogClock& clock, std::uint64_t time, LogLevel level,
                            std::string_view text) {
    return format_line(clock, time, static_cast<std::uint64_t>(level), text, false);
}

std::string format_log_record(const LogClock& clock, const LogRecord& record) {
    // The record is the caller's copy. Validate it before using any of it.
    const bool valid = level_name(record.level).has_value() && record.length <= kLogTextSize &&
                       (record.flags & ~kLogTruncated) == 0;
    if (!valid) {
        std::string line = time_text(clock, record.time);
        line += kInvalidRecord;
        return line;
    }
    // The text's bytes are chars under another name; `length` was checked.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string_view text{reinterpret_cast<const char*>(record.text.data()),
                                static_cast<std::size_t>(record.length)};
    return format_line(clock, record.time, record.level, text, (record.flags & kLogTruncated) != 0);
}

std::string format_log_header(std::string_view process, const LogClock& clock) {
    std::string line = "# ";
    append_escaped(line, process);
    line += " log; session started ";
    line += time_text(clock, clock.steady_ns);
    // The offset's size as an unsigned value, so the lowest one has one too.
    const bool west = clock.utc_offset_s < 0;
    const auto offset = static_cast<std::uint64_t>(clock.utc_offset_s);
    const std::uint64_t size = west ? std::uint64_t{0} - offset : offset;
    constexpr auto kHour = static_cast<std::uint64_t>(kSecondsPerHour);
    constexpr auto kMinute = static_cast<std::uint64_t>(kSecondsPerMinute);
    constexpr auto kNs = static_cast<std::uint64_t>(kNsPerSecond);
    line += std::format(" {}{:02}:{:02}; steady clock {}.{:09}", west ? '-' : '+', size / kHour,
                        (size % kHour) / kMinute, clock.steady_ns / kNs, clock.steady_ns % kNs);
    return line;
}

} // namespace ks::ipc
