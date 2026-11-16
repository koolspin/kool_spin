// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/log.h>

#include <ks/ipc/layout.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

using ks::ipc::kLogRingCapacity;
using ks::ipc::kLogTextSize;
using ks::ipc::kLogTruncated;
using ks::ipc::load_shared;
using ks::ipc::LogLevel;
using ks::ipc::LogReader;
using ks::ipc::LogRecord;
using ks::ipc::LogSegment;
using ks::ipc::LogWriter;
using ks::ipc::store_shared;

/// An empty log, as a new segment holds one. It's too big for the stack.
std::unique_ptr<LogSegment> empty_log() {
    return std::make_unique<LogSegment>();
}

/// The line a record holds.
std::string text_of(const LogRecord& record) {
    REQUIRE(record.length <= kLogTextSize);
    std::string text;
    for (const std::uint8_t byte : std::span{record.text}.first(record.length)) {
        text.push_back(static_cast<char>(byte));
    }
    return text;
}

/// The one record that writing `text` to an empty log makes.
LogRecord written(std::string_view text) {
    const std::unique_ptr<LogSegment> log = empty_log();
    LogWriter writer{*log};
    writer.write(LogLevel::Warning, text);
    LogReader reader{*log};
    const std::optional<LogRecord> record = reader.next();
    REQUIRE(record.has_value());
    REQUIRE_FALSE(reader.next().has_value());
    return record.value_or(LogRecord{});
}

} // namespace

TEST_CASE("LogWriter fills in every field of a record", "[ipc][log]") {
    const LogRecord record = written("hello");
    CHECK(record.level == static_cast<std::uint64_t>(LogLevel::Warning));
    CHECK(record.length == 5);
    CHECK(record.flags == 0);
    CHECK(record.time != 0);
    CHECK(text_of(record) == "hello");
    // The rest of the text is zeroed: nothing from the writer's memory leaks
    // into the segment.
    for (const std::uint8_t byte : std::span{record.text}.subspan(record.length)) {
        CHECK(byte == 0);
    }
}

TEST_CASE("LogWriter writes an empty line", "[ipc][log]") {
    const LogRecord record = written("");
    CHECK(record.length == 0);
    CHECK(record.flags == 0);
}

TEST_CASE("LogWriter cuts at a newline", "[ipc][log]") {
    SECTION("in the middle") {
        const LogRecord record = written("one\ntwo\nthree");
        CHECK(text_of(record) == "one");
        CHECK(record.flags == kLogTruncated);
    }
    SECTION("at the start") {
        const LogRecord record = written("\nrest");
        CHECK(record.length == 0);
        CHECK(record.flags == kLogTruncated);
    }
    SECTION("at the end") {
        const LogRecord record = written("line\n");
        CHECK(text_of(record) == "line");
        CHECK(record.flags == kLogTruncated);
    }
    SECTION("past the size limit") {
        const LogRecord record = written(std::string(300, 'a') + "\nrest");
        CHECK(text_of(record) == std::string(kLogTextSize, 'a'));
        CHECK(record.flags == kLogTruncated);
    }
    SECTION("exactly at the size limit") {
        const LogRecord record = written(std::string(kLogTextSize, 'a') + "\n");
        CHECK(text_of(record) == std::string(kLogTextSize, 'a'));
        CHECK(record.flags == kLogTruncated);
    }
}

TEST_CASE("LogWriter cuts a long line at a character boundary", "[ipc][log]") {
    constexpr std::string_view kEuro = "\xe2\x82\xac";
    constexpr std::string_view kFace = "\xf0\x9f\x98\x80";

    SECTION("a character that ends at the limit is kept") {
        const std::string fits = std::string(kLogTextSize - 3, 'a') + std::string{kEuro};
        const LogRecord whole = written(fits);
        CHECK(text_of(whole) == fits);
        CHECK(whole.flags == 0);

        const LogRecord cut = written(fits + "b");
        CHECK(text_of(cut) == fits);
        CHECK(cut.flags == kLogTruncated);
    }
    SECTION("a character that straddles the limit is left out") {
        for (std::size_t inside = 1; inside < kFace.size(); ++inside) {
            CAPTURE(inside);
            const std::string before(kLogTextSize - inside, 'a');
            const LogRecord record = written(before + std::string{kFace} + "tail");
            CHECK(text_of(record) == before);
            CHECK(record.flags == kLogTruncated);
        }
    }
    SECTION("text that isn't UTF-8 is cut at the limit") {
        const LogRecord record = written(std::string(300, '\x80'));
        CHECK(text_of(record) == std::string(kLogTextSize, '\x80'));
        CHECK(record.flags == kLogTruncated);
    }
}

TEST_CASE("LogWriter continues from the segment's counts", "[ipc][log]") {
    const std::unique_ptr<LogSegment> log = empty_log();
    {
        LogWriter first{*log};
        for (std::size_t n = 0; n < kLogRingCapacity + 3; ++n) {
            first.write(LogLevel::Info, "first");
        }
        CHECK(first.dropped() == 3);
    }
    LogWriter second{*log};
    CHECK(second.dropped() == 3);
    second.write(LogLevel::Info, "second");
    CHECK(second.dropped() == 4);
    CHECK(load_shared(log->dropped, std::memory_order_acquire) == 4);
}

TEST_CASE("LogWriter never reads its counts back from the segment", "[ipc][log]") {
    const std::unique_ptr<LogSegment> log = empty_log();
    LogWriter writer{*log};
    for (std::size_t n = 0; n < kLogRingCapacity + 1; ++n) {
        writer.write(LogLevel::Info, "line");
    }
    REQUIRE(writer.dropped() == 1);
    // Something else scribbles on the count: the writer's next drop puts its
    // own count back.
    store_shared(log->dropped, 1'000, std::memory_order_release);
    writer.write(LogLevel::Info, "line");
    CHECK(writer.dropped() == 2);
    CHECK(load_shared(log->dropped, std::memory_order_acquire) == 2);
}

TEST_CASE("LogWriter counts lines it can't write to a corrupt ring", "[ipc][log]") {
    const std::unique_ptr<LogSegment> log = empty_log();
    store_shared(log->ring.tail, 5'000, std::memory_order_release);
    LogWriter writer{*log};
    writer.write(LogLevel::Error, "lost");
    CHECK(writer.dropped() == 1);
    CHECK(load_shared(log->ring.head, std::memory_order_acquire) == 0);
}

TEST_CASE("LogReader continues from the ring's tail", "[ipc][log]") {
    const std::unique_ptr<LogSegment> log = empty_log();
    LogWriter writer{*log};
    writer.write(LogLevel::Info, "one");
    writer.write(LogLevel::Info, "two");
    {
        LogReader first{*log};
        const std::optional<LogRecord> record = first.next();
        REQUIRE(record.has_value());
        CHECK(text_of(record.value_or(LogRecord{})) == "one");
    }
    LogReader second{*log};
    const std::optional<LogRecord> record = second.next();
    REQUIRE(record.has_value());
    CHECK(text_of(record.value_or(LogRecord{})) == "two");
    CHECK_FALSE(second.next().has_value());
    CHECK_FALSE(second.corrupt());
}

TEST_CASE("LogReader reports drops once each", "[ipc][log]") {
    const std::unique_ptr<LogSegment> log = empty_log();
    LogReader reader{*log};
    CHECK(reader.take_dropped() == 0);
    store_shared(log->dropped, 7, std::memory_order_release);
    CHECK(reader.take_dropped() == 7);
    CHECK(reader.take_dropped() == 0);
    store_shared(log->dropped, 9, std::memory_order_release);
    CHECK(reader.take_dropped() == 2);
    CHECK_FALSE(reader.corrupt());
}

TEST_CASE("LogReader never touches a corrupt segment again", "[ipc][log]") {
    const std::unique_ptr<LogSegment> log = empty_log();
    LogWriter writer{*log};
    writer.write(LogLevel::Info, "unread");

    SECTION("after the dropped count went backwards") {
        LogReader reader{*log};
        store_shared(log->dropped, 5, std::memory_order_release);
        REQUIRE(reader.take_dropped() == 5);
        store_shared(log->dropped, 4, std::memory_order_release);
        CHECK(reader.take_dropped() == 0);
        CHECK(reader.corrupt());
        // A count that looks right again doesn't bring the reader back.
        store_shared(log->dropped, 50, std::memory_order_release);
        CHECK(reader.take_dropped() == 0);
        CHECK_FALSE(reader.next().has_value());
        CHECK(load_shared(log->ring.tail, std::memory_order_acquire) == 0);
    }
    SECTION("after the ring's counters went wrong") {
        LogReader reader{*log};
        store_shared(log->ring.head, 5'000, std::memory_order_release);
        CHECK_FALSE(reader.next().has_value());
        CHECK(reader.corrupt());
        store_shared(log->ring.head, 1, std::memory_order_release);
        store_shared(log->dropped, 6, std::memory_order_release);
        CHECK_FALSE(reader.next().has_value());
        CHECK(reader.take_dropped() == 0);
        CHECK(load_shared(log->ring.tail, std::memory_order_acquire) == 0);
    }
}
