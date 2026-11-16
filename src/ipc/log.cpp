// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/log.h>

#include <ks/ipc/layout.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

// The clock and the formats are still stubs (specs/logs/spec.md, plan task 2).

namespace ks::ipc {

namespace {

/// A line's text as a record holds it.
struct Cut {
    std::size_t length;
    bool truncated;
};

/// True for a UTF-8 continuation byte, 10xxxxxx.
constexpr bool is_continuation(char byte) noexcept {
    return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

/// How much of `text` goes into a record: up to the first newline, and at
/// most kLogTextSize bytes, ending on a UTF-8 character boundary.
Cut cut_line(std::string_view text) noexcept {
    Cut cut{.length = text.size(), .truncated = false};
    if (const std::size_t newline = text.find('\n'); newline != std::string_view::npos) {
        cut = {.length = newline, .truncated = true};
    }
    if (cut.length <= kLogTextSize) {
        return cut;
    }
    // The byte at `length` is the first one left out. While it's a
    // continuation byte, the cut is inside a character: move back to the
    // character's first byte. A character has at most three continuation
    // bytes, so text that needs more steps isn't UTF-8, and is cut at the
    // size limit (format_log_record() escapes whatever is left).
    constexpr std::size_t kMaxContinuations = 3;
    std::size_t length = kLogTextSize;
    while (kLogTextSize - length < kMaxContinuations && is_continuation(text[length])) {
        --length;
    }
    if (is_continuation(text[length])) {
        length = kLogTextSize;
    }
    return {.length = length, .truncated = true};
}

/// std::chrono::steady_clock, in nanoseconds.
std::uint64_t steady_ns() noexcept {
    const auto since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    return static_cast<std::uint64_t>(since_epoch.count());
}

} // namespace

LogWriter::LogWriter(LogSegment& segment) noexcept
    : segment_{&segment}, producer_{segment.ring},
      dropped_{load_shared(segment.dropped, std::memory_order_relaxed)} {}

void LogWriter::write(LogLevel level, std::string_view text) {
    const Cut cut = cut_line(text);
    LogRecord record{};
    record.level = static_cast<std::uint64_t>(level);
    record.length = cut.length;
    record.flags = cut.truncated ? kLogTruncated : 0;
    if (cut.length > 0) {
        std::memcpy(record.text.data(), text.data(), cut.length);
    }

    // The time is read under the mutex, so the ring's records are in time
    // order, whichever threads wrote them.
    const std::scoped_lock lock{mutex_};
    record.time = steady_ns();
    if (!producer_.try_push(record)) {
        // Full, or corrupt: either way the line is lost, and counted. The
        // count comes from private memory, never back from the segment.
        ++dropped_;
        store_shared(segment_->dropped, dropped_, std::memory_order_release);
    }
}

std::uint64_t LogWriter::dropped() const {
    const std::scoped_lock lock{mutex_};
    return dropped_;
}

LogReader::LogReader(LogSegment& segment) noexcept : segment_{&segment}, consumer_{segment.ring} {}

std::optional<LogRecord> LogReader::next() noexcept {
    if (corrupt()) {
        return std::nullopt;
    }
    return consumer_.try_pop();
}

std::uint64_t LogReader::take_dropped() noexcept {
    if (corrupt()) {
        return 0;
    }
    // Read once: the writer's process may change it at any moment.
    const std::uint64_t dropped = load_shared(segment_->dropped, std::memory_order_acquire);
    if (dropped < dropped_) {
        dropped_corrupt_ = true;
        return 0;
    }
    const std::uint64_t since = dropped - dropped_;
    dropped_ = dropped;
    return since;
}

bool LogReader::corrupt() const noexcept {
    return dropped_corrupt_ || consumer_.corrupt();
}

LogClock LogClock::now() {
    return LogClock{};
}

std::string format_log_line(const LogClock& clock, std::uint64_t time, LogLevel level,
                            std::string_view text) {
    static_cast<void>(clock);
    static_cast<void>(time);
    static_cast<void>(level);
    static_cast<void>(text);
    return {};
}

std::string format_log_record(const LogClock& clock, const LogRecord& record) {
    static_cast<void>(clock);
    static_cast<void>(record);
    return {};
}

std::string format_log_header(std::string_view process, const LogClock& clock) {
    static_cast<void>(process);
    static_cast<void>(clock);
    return {};
}

} // namespace ks::ipc
