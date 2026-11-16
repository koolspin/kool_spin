// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/log.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace {

using ks::ipc::format_log_header;
using ks::ipc::format_log_line;
using ks::ipc::format_log_record;
using ks::ipc::kLogTextSize;
using ks::ipc::kLogTruncated;
using ks::ipc::LogClock;
using ks::ipc::LogLevel;
using ks::ipc::LogRecord;

constexpr std::uint64_t kSecond = 1'000'000'000;

/// 10000-01-01 00:00:00, in seconds since 1970.
constexpr std::int64_t kYear10000 = 253'402'300'800;

/// Nanoseconds since 1970 of a UTC date and time.
std::int64_t system_ns(std::chrono::year_month_day date, std::chrono::seconds in_day) {
    const std::chrono::sys_seconds time = std::chrono::sys_days{date} + in_day;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

/// Steady time 1,000 s is 2026-11-15 23:22:03 UTC, in UTC−5.
LogClock test_clock() {
    using std::chrono::November;
    return LogClock{.steady_ns = 1'000 * kSecond,
                    .system_ns = system_ns(std::chrono::year{2026} / November / 15,
                                           std::chrono::hours{23} + std::chrono::minutes{22} +
                                               std::chrono::seconds{3}),
                    .utc_offset_s = std::int64_t{-5} * 3'600};
}

/// A clock whose steady time 1 s is the start of 1970, in UTC.
LogClock epoch_clock(std::int64_t utc_offset_s = 0) {
    return LogClock{.steady_ns = kSecond, .system_ns = 0, .utc_offset_s = utc_offset_s};
}

/// The text of the line `text` makes, after the test clock's time and " info ".
std::string escaped(std::string_view text) {
    const std::string line = format_log_line(test_clock(), 1'000 * kSecond, LogLevel::Info, text);
    constexpr std::string_view kStart = "2026-11-15 18:22:03.000000 info ";
    REQUIRE(line.starts_with(kStart));
    return line.substr(kStart.size());
}

LogRecord record_of(std::uint64_t level, std::string_view text, std::uint64_t flags = 0) {
    LogRecord record{};
    record.time = 1'000 * kSecond;
    record.level = level;
    record.length = text.size();
    record.flags = flags;
    std::size_t i = 0;
    for (const char c : text) {
        std::span{record.text}[i] = static_cast<std::uint8_t>(c);
        ++i;
    }
    return record;
}

} // namespace

TEST_CASE("Log lines show times before and after the clock was read", "[ipc][log]") {
    const LogClock clock = test_clock();
    CHECK(format_log_line(clock, 999 * kSecond, LogLevel::Info, "x") ==
          "2026-11-15 18:22:02.000000 info x");
    // One nanosecond before the clock: rounded down, not toward the clock.
    CHECK(format_log_line(clock, (1'000 * kSecond) - 1, LogLevel::Info, "x") ==
          "2026-11-15 18:22:02.999999 info x");
    CHECK(format_log_line(clock, 0, LogLevel::Info, "x") == "2026-11-15 18:05:23.000000 info x");
    // Past midnight, local time.
    CHECK(format_log_line(clock, (1'000 + 6 * 3'600) * kSecond, LogLevel::Info, "x") ==
          "2026-11-16 00:22:03.000000 info x");
}

TEST_CASE("Log lines show leap days", "[ipc][log]") {
    using std::chrono::February;
    const LogClock clock{.steady_ns = 0,
                         .system_ns = system_ns(std::chrono::year{2028} / February / 28,
                                                std::chrono::hours{24} - std::chrono::seconds{1}),
                         .utc_offset_s = 0};
    CHECK(format_log_line(clock, kSecond, LogLevel::Info, "x") ==
          "2028-02-29 00:00:00.000000 info x");
    CHECK(format_log_line(clock, (1 + 86'400) * kSecond, LogLevel::Info, "x") ==
          "2028-03-01 00:00:00.000000 info x");
}

TEST_CASE("Log lines show only the years 1970 to 9999", "[ipc][log]") {
    SECTION("the start of 1970") {
        CHECK(format_log_line(epoch_clock(), kSecond, LogLevel::Info, "x") ==
              "1970-01-01 00:00:00.000000 info x");
        CHECK(format_log_line(epoch_clock(), kSecond - 1, LogLevel::Info, "x") ==
              "steady:999999999 info x");
        // It's the local time that is shown, and that has to fit.
        CHECK(format_log_line(epoch_clock(-1), kSecond, LogLevel::Info, "x") ==
              "steady:1000000000 info x");
        CHECK(format_log_line(epoch_clock(3'600), 0, LogLevel::Info, "x") ==
              "1970-01-01 00:59:59.000000 info x");
    }
    SECTION("a system time before 1970 rounds down") {
        const LogClock clock{.steady_ns = 0, .system_ns = -1, .utc_offset_s = 1};
        CHECK(format_log_line(clock, 0, LogLevel::Info, "x") ==
              "1970-01-01 00:00:00.999999 info x");
    }
    SECTION("the end of 9999") {
        CHECK(format_log_line(epoch_clock(kYear10000 - 1), kSecond + 999'999'999, LogLevel::Info,
                              "x") == "9999-12-31 23:59:59.999999 info x");
        CHECK(format_log_line(epoch_clock(kYear10000 - 1), 2 * kSecond, LogLevel::Info, "x") ==
              "steady:2000000000 info x");
        CHECK(format_log_line(epoch_clock(kYear10000), kSecond, LogLevel::Info, "x") ==
              "steady:1000000000 info x");
    }
    SECTION("the extremes don't overflow") {
        constexpr std::uint64_t kMaxTime = std::numeric_limits<std::uint64_t>::max();
        constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
        constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
        for (const std::int64_t system : {kMin, std::int64_t{0}, kMax}) {
            for (const std::int64_t offset : {kMin, std::int64_t{0}, kMax}) {
                const LogClock early{.steady_ns = 0, .system_ns = system, .utc_offset_s = offset};
                const LogClock late{
                    .steady_ns = kMaxTime, .system_ns = system, .utc_offset_s = offset};
                CHECK(!format_log_line(early, kMaxTime, LogLevel::Info, "x").empty());
                CHECK(!format_log_line(late, 0, LogLevel::Info, "x").empty());
                CHECK(!format_log_header("p", early).empty());
                CHECK(!format_log_header("p", late).empty());
            }
        }
        // The latest system time, 2262-04-11, moved by the longest steady
        // difference, 584 years: 9,223,372,036.854775807 s and
        // 18,446,744,073.709551615 s make 27,670,116,110.564327422 s, which
        // is 320,255 days and 84,110 s after the start of 1970.
        const LogClock clock{.steady_ns = 0, .system_ns = kMax, .utc_offset_s = 0};
        CHECK(format_log_line(clock, kMaxTime, LogLevel::Info, "x") ==
              "2846-10-30 23:21:50.564327 info x");
        CHECK(format_log_line(LogClock{.steady_ns = 0, .system_ns = 0, .utc_offset_s = kMin}, 5,
                              LogLevel::Info, "x") == "steady:5 info x");
    }
}

TEST_CASE("Log lines escape control characters and invalid UTF-8", "[ipc][log]") {
    SECTION("what is copied") {
        CHECK(escaped("plain text") == "plain text");
        CHECK(escaped("").empty());
        // A backslash isn't escaped: the text is copied as it is.
        CHECK(escaped(R"(a\x41b)") == R"(a\x41b)");
        CHECK(escaped("~") == "~");
        CHECK(escaped("\xc2\xa0") == "\xc2\xa0");                 // U+00A0
        CHECK(escaped("\xdf\xbf") == "\xdf\xbf");                 // U+07FF
        CHECK(escaped("\xe0\xa0\x80") == "\xe0\xa0\x80");         // U+0800
        CHECK(escaped("\xed\x9f\xbf") == "\xed\x9f\xbf");         // U+D7FF
        CHECK(escaped("\xee\x80\x80") == "\xee\x80\x80");         // U+E000
        CHECK(escaped("\xf0\x90\x80\x80") == "\xf0\x90\x80\x80"); // U+10000
        CHECK(escaped("\xf4\x8f\xbf\xbf") == "\xf4\x8f\xbf\xbf"); // U+10FFFF
    }
    SECTION("control characters") {
        CHECK(escaped("\x1f") == R"(\x1f)");
        CHECK(escaped("\r\n") == R"(\x0d\x0a)");
        CHECK(escaped("\xc2\x80") == R"(\xc2\x80)"); // U+0080
        CHECK(escaped("\xc2\x9f") == R"(\xc2\x9f)"); // U+009F
    }
    SECTION("invalid UTF-8") {
        CHECK(escaped("\x80") == R"(\x80)");                         // a stray continuation
        CHECK(escaped("\xc0\x80") == R"(\xc0\x80)");                 // overlong U+0000
        CHECK(escaped("\xc1\xbf") == R"(\xc1\xbf)");                 // overlong U+007F
        CHECK(escaped("\xe0\x9f\xbf") == R"(\xe0\x9f\xbf)");         // overlong U+07FF
        CHECK(escaped("\xf0\x8f\xbf\xbf") == R"(\xf0\x8f\xbf\xbf)"); // overlong U+FFFF
        CHECK(escaped("\xed\xa0\x80") == R"(\xed\xa0\x80)");         // U+D800, a surrogate
        CHECK(escaped("\xed\xbf\xbf") == R"(\xed\xbf\xbf)");         // U+DFFF
        CHECK(escaped("\xf4\x90\x80\x80") == R"(\xf4\x90\x80\x80)"); // U+110000
        CHECK(escaped("\xf5\x80\x80\x80") == R"(\xf5\x80\x80\x80)");
        CHECK(escaped("\xf0\x9f\x98") == R"(\xf0\x9f\x98)"); // cut short at the end
    }
    SECTION("a valid character after an invalid byte is kept") {
        CHECK(escaped("\xe2\xc3\xa9") == "\\xe2\xc3\xa9");
        CHECK(escaped("\xc3"
                      "A") == R"(\xc3A)");
    }
}

TEST_CASE("Log records are validated before they're formatted", "[ipc][log]") {
    const LogClock clock = test_clock();
    const std::string time = "2026-11-15 18:22:03.000000";
    CHECK(format_log_record(clock, record_of(2, "odd")) == time + " warning odd");
    CHECK(format_log_record(clock, record_of(3, "")) == time + " info ");
    CHECK(format_log_record(clock, record_of(3, "", kLogTruncated)) == time + " info \xe2\x80\xa6");
    // Bytes after `length` aren't part of the line.
    LogRecord record = record_of(3, "abcdef");
    record.length = 3;
    CHECK(format_log_record(clock, record) == time + " info abc");
    // A full record, and one byte more than fits.
    const std::string full(kLogTextSize, 'x');
    CHECK(format_log_record(clock, record_of(1, full)) == time + " error " + full);
    record = record_of(1, full);
    record.length = kLogTextSize + 1;
    CHECK(format_log_record(clock, record) == time + " warning [invalid log record]");
    record.length = std::numeric_limits<std::uint64_t>::max();
    CHECK(format_log_record(clock, record) == time + " warning [invalid log record]");
    // Values that would pass if only their low bits were looked at.
    CHECK(format_log_record(clock, record_of((std::uint64_t{1} << 32U) + 1, "x")) ==
          time + " warning [invalid log record]");
    CHECK(format_log_record(clock, record_of(3, "x", (std::uint64_t{1} << 63U) | kLogTruncated)) ==
          time + " warning [invalid log record]");
    // An invalid record at a time that can't be shown.
    LogRecord early = record_of(0, "x");
    early.time = 7;
    CHECK(format_log_record(epoch_clock(), early) == "steady:7 warning [invalid log record]");
}

TEST_CASE("A level that isn't one makes an invalid line", "[ipc][log]") {
    CHECK(format_log_line(test_clock(), 1'000 * kSecond, static_cast<LogLevel>(0), "x") ==
          "2026-11-15 18:22:03.000000 warning [invalid log record]");
}

TEST_CASE("Log headers show the clock", "[ipc][log]") {
    LogClock clock = test_clock();
    clock.steady_ns += 123'456'789;
    clock.system_ns += 123'456'789;
    CHECK(format_log_header("kool_spin", clock) ==
          "# kool_spin log; session started 2026-11-15 18:22:03.123456 -05:00; steady clock "
          "1000.123456789");
    clock.utc_offset_s = 0;
    CHECK(format_log_header("kool_spin", clock) ==
          "# kool_spin log; session started 2026-11-15 23:22:03.123456 +00:00; steady clock "
          "1000.123456789");
    clock.utc_offset_s = -1'800;
    CHECK(format_log_header("kool_spin", clock) ==
          "# kool_spin log; session started 2026-11-15 22:52:03.123456 -00:30; steady clock "
          "1000.123456789");
    // A clock that can't be shown still makes a header, and a process name is
    // escaped like any text.
    const LogClock wild{
        .steady_ns = 5, .system_ns = 0, .utc_offset_s = std::numeric_limits<std::int64_t>::min()};
    CHECK(format_log_header("a\nb", wild) == "# a\\x0ab log; session started steady:5 "
                                             "-2562047788015215:30; steady clock 0.000000005");
}

TEST_CASE("LogClock::now() reads the clocks", "[ipc][log]") {
    const auto steady_now = [] {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    };
    const auto system_now = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
    const std::uint64_t steady_before = steady_now();
    const std::int64_t system_before = system_now();
    const LogClock clock = LogClock::now();
    CHECK(clock.steady_ns >= steady_before);
    CHECK(clock.steady_ns <= steady_now());
    // The wall clock may be stepped during a test, but not by a minute.
    constexpr std::int64_t kMinute = 60 * static_cast<std::int64_t>(kSecond);
    CHECK(clock.system_ns >= system_before - kMinute);
    CHECK(clock.system_ns <= system_now() + kMinute);
    // Real offsets run from UTC−12 to UTC+14.
    CHECK(clock.utc_offset_s >= std::int64_t{-18} * 3'600);
    CHECK(clock.utc_offset_s <= std::int64_t{18} * 3'600);
    // A line written now can be shown.
    CHECK(!format_log_line(clock, steady_now(), LogLevel::Info, "x").starts_with("steady:"));
}
