// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/logs/spec.md: the log segment, the writer and
// reader, the line and header formats, and log files (requirements 1 to 8).

#include <ks/ipc/log.h>

#include <ks/ipc/fd.h>
#include <ks/ipc/layout.h>
#include <ks/ipc/region.h>
#include <ks/ipc/segment.h>
#include <ks/supervisor/log_file.h>

#include "logs_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using ks::ipc::LogClock;
using ks::ipc::LogLevel;
using ks::ipc::LogReader;
using ks::ipc::LogRecord;
using ks::ipc::LogSegment;
using ks::ipc::LogWriter;
using ks::logs::test::check_record_input;
using ks::logs::test::make_record;
using ks::logs::test::mode_of;
using ks::logs::test::parse_line;
using ks::logs::test::private_segment;
using ks::logs::test::read_lines;
using ks::logs::test::steady_now;
using ks::logs::test::text_of;
using ks::logs::test::well_formed;

/// The levels the threads in "lines in order" cycle through.
constexpr std::array<LogLevel, 3> kLevels{LogLevel::Error, LogLevel::Warning, LogLevel::Info};

/// "thread <t> line <n>" as {t, n}, or nothing.
std::optional<std::pair<int, int>> thread_line(std::string_view text) {
    constexpr std::string_view kThread = "thread ";
    constexpr std::string_view kLine = " line ";
    const std::size_t middle = text.find(kLine);
    if (!text.starts_with(kThread) || middle == std::string_view::npos) {
        return std::nullopt;
    }
    const auto whole_number = [](std::string_view digits) -> std::optional<int> {
        const char* const first = std::to_address(digits.begin());
        const char* const last = std::to_address(digits.end());
        int value = -1;
        const auto [end, error] = std::from_chars(first, last, value);
        if (error != std::errc{} || end != last) {
            return std::nullopt;
        }
        return value;
    };
    const std::optional<int> t = whole_number(text.substr(kThread.size(), middle - kThread.size()));
    const std::optional<int> n = whole_number(text.substr(middle + kLine.size()));
    if (!t || !n) {
        return std::nullopt;
    }
    return std::pair{*t, *n};
}

/// Steady-clock nanoseconds for `seconds`.
constexpr std::uint64_t seconds_ns(std::uint64_t seconds) {
    return seconds * 1'000'000'000U;
}

/// The clock "Log lines: format" uses: steady time 1,000 s, system time
/// 2026-11-15 23:22:03 UTC, and UTC−5.
LogClock test_clock() {
    const std::chrono::sys_seconds system =
        std::chrono::sys_days{std::chrono::year{2026} / std::chrono::November / 15} +
        std::chrono::hours{23} + std::chrono::minutes{22} + std::chrono::seconds{3};
    return LogClock{
        .steady_ns = seconds_ns(1'000),
        .system_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(system.time_since_epoch()).count(),
        .utc_offset_s = std::int64_t{-5} * 3'600};
}

/// Writes `text` through a fresh writer on a fresh segment, and returns the
/// one record it made.
std::optional<LogRecord> written(std::string_view text) {
    const std::unique_ptr<LogSegment> segment = private_segment();
    LogWriter writer{*segment};
    writer.write(LogLevel::Info, text);
    LogReader reader{*segment};
    return reader.next();
}

/// `count` copies of `piece`.
std::string repeat(std::string_view piece, std::size_t count) {
    std::string text;
    for (std::size_t i = 0; i < count; ++i) {
        text += piece;
    }
    return text;
}

} // namespace

TEST_CASE("Log segments: layout", "[segment]") {
    STATIC_CHECK(sizeof(LogRecord) == 256);
    STATIC_CHECK(ks::ipc::kLogTextSize == 224);
    STATIC_CHECK(ks::ipc::kLogRingCapacity == 1'024);
    STATIC_CHECK(ks::ipc::kLogLayoutVersion == 1);
    STATIC_CHECK(ks::ipc::kLogSegmentSize == ks::ipc::kSegmentHeaderSize + sizeof(LogSegment));
    STATIC_CHECK(static_cast<int>(LogLevel::Error) == 1);
    STATIC_CHECK(static_cast<int>(LogLevel::Warning) == 2);
    STATIC_CHECK(static_cast<int>(LogLevel::Info) == 3);
    STATIC_CHECK(ks::ipc::kLogTruncated == 1);

    const ks::ipc::test::TempDir dir;
    REQUIRE(!dir.path().empty());
    std::expected<ks::ipc::SegmentFactory, std::error_code> factory =
        ks::ipc::SegmentFactory::open(dir.path());
    REQUIRE(factory.has_value());
    const ks::logs::test::MappedLog log = ks::logs::test::create_log(*factory);
    CHECK(ks::ipc::load_shared(log.segment->dropped, std::memory_order_acquire) == 0);
    LogReader reader{*log.segment};
    CHECK(!reader.next().has_value());
    CHECK(reader.take_dropped() == 0);
    CHECK(!reader.corrupt());
}

TEST_CASE("Log writer: lines in order, from many threads", "[writer]") {
    constexpr int kThreads = 4;
    constexpr int kLines = 200;
    constexpr std::size_t kTotal = std::size_t{kThreads} * std::size_t{kLines};
    const std::unique_ptr<LogSegment> segment = private_segment();
    LogWriter writer{*segment};
    LogReader reader{*segment};

    const std::uint64_t before = steady_now();
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&writer, t] {
            for (int n = 0; n < kLines; ++n) {
                writer.write(std::span{kLevels}[static_cast<std::size_t>(n % 3)],
                             std::format("thread {} line {}", t, n));
            }
        });
    }
    std::vector<LogRecord> records;
    static_cast<void>(ks::logs::test::eventually(
        [&] {
            while (const std::optional<LogRecord> record = reader.next()) {
                records.push_back(*record);
            }
            return records.size() >= kTotal;
        },
        std::chrono::milliseconds{10'000}));
    for (std::thread& thread : threads) {
        thread.join();
    }
    while (const std::optional<LogRecord> record = reader.next()) {
        records.push_back(*record);
    }
    const std::uint64_t after = steady_now();

    REQUIRE(records.size() == kTotal);
    std::map<int, int> next_line;
    std::map<int, std::uint64_t> last_time;
    for (const LogRecord& record : records) {
        const std::string text = text_of(record);
        CAPTURE(text);
        const std::optional<std::pair<int, int>> parsed = thread_line(text);
        REQUIRE(parsed.has_value());
        const auto [t, n] = parsed.value_or(std::pair{-1, -1});
        CHECK(n == next_line[t]);
        next_line[t] = n + 1;
        CHECK(record.level ==
              static_cast<std::uint64_t>(std::span{kLevels}[static_cast<std::size_t>(n % 3)]));
        CHECK(record.flags == 0);
        CHECK(record.time >= last_time[t]);
        last_time[t] = record.time;
        CHECK(record.time >= before);
        CHECK(record.time <= after);
    }
    for (int t = 0; t < kThreads; ++t) {
        CHECK(next_line[t] == kLines);
    }
    CHECK(reader.take_dropped() == 0);
    CHECK(writer.dropped() == 0);
}

TEST_CASE("Log writer: a full ring drops lines and counts them", "[writer]") {
    const std::unique_ptr<LogSegment> segment = private_segment();
    LogWriter writer{*segment};
    const std::size_t total = ks::ipc::kLogRingCapacity + 10;
    for (std::size_t n = 0; n < total; ++n) {
        writer.write(LogLevel::Info, std::format("line {}", n));
    }
    CHECK(writer.dropped() == 10);
    CHECK(ks::ipc::load_shared(segment->dropped, std::memory_order_acquire) == 10);

    LogReader reader{*segment};
    CHECK(reader.take_dropped() == 10);
    CHECK(reader.take_dropped() == 0);
    for (std::size_t n = 0; n < ks::ipc::kLogRingCapacity; ++n) {
        const std::optional<LogRecord> record = reader.next();
        REQUIRE(record.has_value());
        CHECK(text_of(record.value_or(LogRecord{})) == std::format("line {}", n));
    }
    CHECK(!reader.next().has_value());

    writer.write(LogLevel::Warning, "after");
    const std::optional<LogRecord> after = reader.next();
    REQUIRE(after.has_value());
    CHECK(text_of(after.value_or(LogRecord{})) == "after");
    CHECK(after.value_or(LogRecord{}).level == static_cast<std::uint64_t>(LogLevel::Warning));
    CHECK(!reader.corrupt());
}

TEST_CASE("Log writer: long lines are truncated", "[writer]") {
    struct Case {
        std::string name;
        std::string text;
        std::string expected;
        bool truncated;
    };
    const std::vector<Case> cases{
        {.name = "300 ASCII bytes",
         .text = std::string(300, 'a'),
         .expected = std::string(224, 'a'),
         .truncated = true},
        {.name = "150 two-byte characters",
         .text = repeat("\xc3\xa9", 150),
         .expected = repeat("\xc3\xa9", 112),
         .truncated = true},
        {.name = "100 three-byte characters",
         .text = repeat("\xe2\x82\xac", 100),
         .expected = repeat("\xe2\x82\xac", 74),
         .truncated = true},
        {.name = "60 four-byte characters",
         .text = repeat("\xf0\x9f\x98\x80", 60),
         .expected = repeat("\xf0\x9f\x98\x80", 56),
         .truncated = true},
        {.name = "224 bytes",
         .text = std::string(224, 'b'),
         .expected = std::string(224, 'b'),
         .truncated = false},
        {.name = "a newline", .text = "one\ntwo", .expected = "one", .truncated = true},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        const std::optional<LogRecord> record = written(c.text);
        REQUIRE(record.has_value());
        const LogRecord got = record.value_or(LogRecord{});
        CHECK(text_of(got) == c.expected);
        CHECK(got.length == c.expected.size());
        CHECK(got.flags == (c.truncated ? ks::ipc::kLogTruncated : 0));
        CHECK(got.level == static_cast<std::uint64_t>(LogLevel::Info));
    }
}

TEST_CASE("Log reader: a corrupt ring", "[reader]") {
    SECTION("the head too far past the tail") {
        const std::unique_ptr<LogSegment> segment = private_segment();
        const std::uint64_t tail =
            ks::ipc::load_shared(segment->ring.tail, std::memory_order_acquire);
        ks::ipc::store_shared(segment->ring.head, tail + 2'000, std::memory_order_release);
        LogReader reader{*segment};
        CHECK(!reader.next().has_value());
        CHECK(reader.corrupt());
    }

    SECTION("the dropped count going backwards") {
        const std::unique_ptr<LogSegment> segment = private_segment();
        LogReader reader{*segment};
        ks::ipc::store_shared(segment->dropped, 5, std::memory_order_release);
        CHECK(reader.take_dropped() == 5);
        CHECK(!reader.corrupt());
        ks::ipc::store_shared(segment->dropped, 3, std::memory_order_release);
        CHECK(reader.take_dropped() == 0);
        CHECK(reader.corrupt());
        LogWriter writer{*segment};
        writer.write(LogLevel::Info, "unread");
        CHECK(!reader.next().has_value());
    }
}

TEST_CASE("Log lines: format", "[format]") {
    const LogClock clock = test_clock();
    const std::uint64_t at = seconds_ns(1'000) + 500'000'000;

    SECTION("lines and levels") {
        CHECK(ks::ipc::format_log_line(clock, at, LogLevel::Info, "hello") ==
              "2026-11-15 18:22:03.500000 info hello");
        CHECK(ks::ipc::format_log_line(clock, seconds_ns(1'000) + 999, LogLevel::Info, "x") ==
              "2026-11-15 18:22:03.000000 info x");
        CHECK(ks::ipc::format_log_line(clock, at, LogLevel::Error, "bad") ==
              "2026-11-15 18:22:03.500000 error bad");
        CHECK(ks::ipc::format_log_line(clock, at, LogLevel::Warning, "odd") ==
              "2026-11-15 18:22:03.500000 warning odd");
        // A minute and a day later: the date moves too.
        CHECK(ks::ipc::format_log_line(clock, seconds_ns(1'000 + 86'400 + 60), LogLevel::Info,
                                       "later") == "2026-11-16 18:23:03.000000 info later");
    }

    SECTION("escapes") {
        struct Case {
            std::string text;
            std::string shown;
        };
        const std::vector<Case> cases{
            {.text = "a\tb", .shown = R"(a\x09b)"},
            {.text = "\x7f", .shown = R"(\x7f)"},
            {.text = "\xc2\x85", .shown = R"(\xc2\x85)"},
            {.text = "\xff", .shown = R"(\xff)"},
            {.text = "\xe2\x82", .shown = R"(\xe2\x82)"},
            {.text = std::string{"nul\0here", 8}, .shown = R"(nul\x00here)"},
            {.text = "\xc3\xa9", .shown = "\xc3\xa9"},
            {.text = "\xe2\x80\xa6", .shown = "\xe2\x80\xa6"},
        };
        for (const Case& c : cases) {
            CAPTURE(c.shown);
            CHECK(ks::ipc::format_log_line(clock, at, LogLevel::Info, c.text) ==
                  "2026-11-15 18:22:03.500000 info " + c.shown);
            CHECK(ks::ipc::format_log_record(
                      clock, make_record(at, static_cast<std::uint64_t>(LogLevel::Info), c.text)) ==
                  "2026-11-15 18:22:03.500000 info " + c.shown);
        }
    }

    SECTION("records") {
        CHECK(ks::ipc::format_log_record(clock, make_record(at, 1, "boom")) ==
              "2026-11-15 18:22:03.500000 error boom");
        CHECK(
            ks::ipc::format_log_record(clock, make_record(at, 3, "cut", ks::ipc::kLogTruncated)) ==
            "2026-11-15 18:22:03.500000 info cut\xe2\x80\xa6");
        const std::string invalid = "2026-11-15 18:22:03.500000 warning [invalid log record]";
        CHECK(ks::ipc::format_log_record(clock, make_record(at, 0, "level 0")) == invalid);
        CHECK(ks::ipc::format_log_record(clock, make_record(at, 4, "level 4")) == invalid);
        CHECK(ks::ipc::format_log_record(clock, make_record(at, 3, "flags 2", 2)) == invalid);
        LogRecord long_record = make_record(at, 3, "long");
        long_record.length = ks::ipc::kLogTextSize + 1;
        CHECK(ks::ipc::format_log_record(clock, long_record) == invalid);
    }

    SECTION("times that can't be shown") {
        const LogClock far{.steady_ns = std::uint64_t{1} << 63U,
                           .system_ns = clock.system_ns,
                           .utc_offset_s = clock.utc_offset_s};
        CHECK(ks::ipc::format_log_line(far, 0, LogLevel::Info, "early") == "steady:0 info early");
        CHECK(ks::ipc::format_log_record(far, make_record(0, 2, "early")) ==
              "steady:0 warning early");
    }

    SECTION("the header") {
        CHECK(ks::ipc::format_log_header("ks-engine", clock) ==
              "# ks-engine log; session started 2026-11-15 18:22:03.000000 -05:00; steady clock "
              "1000.000000000");
        const LogClock east{.steady_ns = seconds_ns(12) + 345,
                            .system_ns = clock.system_ns,
                            .utc_offset_s = (5 * 3'600) + (30 * 60)};
        CHECK(ks::ipc::format_log_header("kool_spin", east) ==
              "# kool_spin log; session started 2026-11-16 04:52:03.000000 +05:30; steady clock "
              "12.000000345");
    }

    SECTION("whatever the record holds") {
        // The fuzz target's check, on fixed and random inputs.
        CHECK(check_record_input({}));
        std::array<std::uint8_t, sizeof(LogRecord) + 24> bytes{};
        bytes.fill(0xFF);
        CHECK(check_record_input(bytes));
        std::mt19937_64 random{20261115};
        for (int i = 0; i < 2'000; ++i) {
            for (std::uint8_t& byte : bytes) {
                byte = static_cast<std::uint8_t>(random());
            }
            // Half the inputs get a valid level and length, so their text is
            // formatted too.
            if (i % 2 == 0) {
                std::span{bytes}[8] = static_cast<std::uint8_t>(1 + ((i / 2) % 3));
                std::ranges::fill(std::span{bytes}.subspan(9, 7), 0);
                std::span{bytes}[16] = static_cast<std::uint8_t>(random() % 225);
                std::ranges::fill(std::span{bytes}.subspan(17, 7), 0);
                std::span{bytes}[24] = static_cast<std::uint8_t>(random() % 2);
                std::ranges::fill(std::span{bytes}.subspan(25, 7), 0);
            }
            CAPTURE(i);
            CHECK(check_record_input(bytes));
        }
    }
}

TEST_CASE("Log files: the size limit", "[file]") {
    const ks::ipc::test::TempDir dir;
    REQUIRE(!dir.path().empty());
    const std::filesystem::path path = dir.path() / "test.log";
    constexpr std::uint64_t kLimit = 4'096;
    const LogClock clock = test_clock();
    std::expected<ks::supervisor::LogFile, std::error_code> file =
        ks::supervisor::LogFile::create(path, "test", clock, kLimit);
    REQUIRE(file.has_value());
    CHECK(mode_of(path) == 0600U);

    int written = 0;
    for (int n = 0; n < 1'000; ++n) {
        if (!file->write(seconds_ns(1'000) + static_cast<std::uint64_t>(n), LogLevel::Info,
                         std::format("line {} of the test, padded to some length", n))) {
            break;
        }
        ++written;
    }
    REQUIRE(written > 10);
    REQUIRE(written < 1'000);
    const std::vector<std::string> lines = read_lines(path);
    REQUIRE(lines.size() == static_cast<std::size_t>(written) + 2);
    CHECK(well_formed(lines, "test"));
    const std::optional<ks::logs::test::Line> last = parse_line(lines.back());
    REQUIRE(last.has_value());
    CHECK(last.value_or(ks::logs::test::Line{}).level == "warning");
    CHECK(!last.value_or(ks::logs::test::Line{}).text.starts_with("line "));
    const std::uintmax_t size = std::filesystem::file_size(path);
    CHECK(size - (lines.back().size() + 1) <= kLimit);

    CHECK(!file->write(seconds_ns(2'000), LogLevel::Error, "dropped"));
    CHECK(!file->write(make_record(seconds_ns(2'000), 1, "dropped too")));
    CHECK(std::filesystem::file_size(path) == size);
    CHECK(!file->error().has_value());

    const std::expected<ks::supervisor::LogFile, std::error_code> again =
        ks::supervisor::LogFile::create(path, "test", clock, kLimit);
    CHECK(!again.has_value());
    CHECK(std::filesystem::file_size(path) == size);
}
