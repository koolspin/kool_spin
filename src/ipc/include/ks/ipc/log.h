// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Log segments: a child process's log lines, in a ring that the supervisor
/// reads and writes to the process's log file (ADR 0003 §13,
/// specs/logs/spec.md requirements 1 to 7).
///
/// A child writes through a LogWriter, from any thread except the audio
/// thread. The supervisor reads through a LogReader, and formats each record
/// with format_log_record(), which validates it: nothing in a record can put
/// anything but one well-formed line into a file.

#include <ks/ipc/layout.h>
#include <ks/ipc/region.h>
#include <ks/ipc/spsc_ring.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace ks::ipc {

/// The layout version in a log segment's header.
inline constexpr std::uint32_t kLogLayoutVersion = 1;

/// How serious a line is. The values are part of the layout.
enum class LogLevel : std::uint8_t {
    Error = 1,
    Warning = 2,
    Info = 3,
};

/// The most bytes of text a record holds.
inline constexpr std::size_t kLogTextSize = 224;

/// LogRecord::flags: the line was cut short to fit.
inline constexpr std::uint64_t kLogTruncated = std::uint64_t{1} << 0U;

/// One line. Another process writes it: copy it out, then validate it
/// (format_log_record() does).
struct LogRecord {
    /// When the line was written: nanoseconds of std::chrono::steady_clock,
    /// which every process on the machine shares.
    std::uint64_t time;
    /// A LogLevel value.
    std::uint64_t level;
    /// Bytes of `text` that hold the line: at most kLogTextSize.
    std::uint64_t length;
    /// kLogTruncated, and no other bits.
    std::uint64_t flags;
    /// The line, in UTF-8, with no newline.
    std::array<std::uint8_t, kLogTextSize> text;
};

static_assert(sizeof(LogRecord) == 256, "a log record is 256 bytes");
static_assert(std::is_trivially_copyable_v<LogRecord>, "log records are copied as bytes");

/// Records a log ring holds.
inline constexpr std::size_t kLogRingCapacity = 1'024;

/// One process's lines, oldest first.
using LogRing = SpscRing<LogRecord, kLogRingCapacity>;

/// What a log segment holds after its header. Zero-filled memory is an empty
/// log.
struct LogSegment {
    LogRing ring;
    /// Lines the writer dropped because the ring was full. Written only by
    /// the writer.
    alignas(kSharedAlignment) std::uint64_t dropped;
};

/// A log segment's size: the header, then a LogSegment at
/// kSegmentHeaderSize.
inline constexpr std::size_t kLogSegmentSize = kSegmentHeaderSize + sizeof(LogSegment);

static_assert(kSegmentHeaderSize % alignof(LogSegment) == 0, "a log segment's log is aligned");

/// The writing side of a log (requirement 3).
class LogWriter {
public:
    /// Attaches to `segment`, which must outlive the writer, and continues
    /// from its ring's head and its `dropped`.
    explicit LogWriter(LogSegment& segment) noexcept;

    LogWriter(const LogWriter&) = delete;
    LogWriter& operator=(const LogWriter&) = delete;
    LogWriter(LogWriter&&) = delete;
    LogWriter& operator=(LogWriter&&) = delete;
    ~LogWriter() = default;

    /// Stamps the line with std::chrono::steady_clock and pushes it as one
    /// record. A line longer than kLogTextSize bytes is cut at the last UTF-8
    /// character boundary that fits, and so is one with a newline, at the
    /// newline; either gets kLogTruncated. When the ring is full, the line
    /// is dropped and counted. Never waits for the reader.
    ///
    /// Any thread may call it except the audio thread: calls are serialized
    /// by a mutex, so each thread's lines keep their order.
    void write(LogLevel level, std::string_view text);

    /// Lines dropped so far, the segment's count included.
    [[nodiscard]] std::uint64_t dropped() const;

private:
    mutable std::mutex mutex_;
    LogSegment* segment_;
    RingProducer<LogRecord, kLogRingCapacity> producer_;
    std::uint64_t dropped_ = 0; ///< Authoritative; only ever written to the segment.
};

/// The reading side of a log (requirement 4).
class LogReader {
public:
    /// Attaches to `segment`, which must outlive the reader, and continues
    /// from its ring's tail.
    explicit LogReader(LogSegment& segment) noexcept;

    LogReader(const LogReader&) = delete;
    LogReader& operator=(const LogReader&) = delete;
    LogReader(LogReader&&) noexcept = default;
    LogReader& operator=(LogReader&&) noexcept = default;
    ~LogReader() = default;

    /// Removes the oldest record and returns a copy of it, or nothing if the
    /// ring is empty or corrupt. The record isn't validated.
    [[nodiscard]] std::optional<LogRecord> next() noexcept;

    /// How many lines were dropped since the last call, or in all on the
    /// first call. A count lower than the last one read makes the reader
    /// corrupt, and returns 0.
    [[nodiscard]] std::uint64_t take_dropped() noexcept;

    /// True once the ring's counters, or the dropped count, have been found
    /// inconsistent. From then on the reader never touches the segment.
    [[nodiscard]] bool corrupt() const noexcept;

private:
    LogSegment* segment_;
    RingConsumer<LogRecord, kLogRingCapacity> consumer_;
    std::uint64_t dropped_ = 0; ///< The dropped count last read.
    bool dropped_corrupt_ = false;
};

/// The steady clock and the wall clock, read at one moment (requirement 5).
/// A session reads it once, and converts every line's time with it.
struct LogClock {
    /// std::chrono::steady_clock, in nanoseconds.
    std::uint64_t steady_ns = 0;
    /// std::chrono::system_clock: nanoseconds since 1970-01-01 00:00:00 UTC.
    std::int64_t system_ns = 0;
    /// The local time's offset from UTC, in seconds: −18,000 for UTC−5.
    std::int64_t utc_offset_s = 0;

    /// Reads all three now.
    [[nodiscard]] static LogClock now();

    friend bool operator==(const LogClock&, const LogClock&) = default;
};

/// One line, without a newline (requirement 6):
/// "YYYY-MM-DD HH:MM:SS.ffffff <level> <text>", with `time` (steady-clock
/// nanoseconds) converted with `clock` and shown in its local time, and
/// control characters and invalid UTF-8 in `text` escaped as \xNN. A time
/// outside the years 1970 to 9999 is shown as "steady:<time>".
[[nodiscard]] std::string format_log_line(const LogClock& clock, std::uint64_t time, LogLevel level,
                                          std::string_view text);

/// `record` as one line, as format_log_line() makes it: with "…" after a
/// truncated record's text, and "<time> warning [invalid log record]" for a
/// record whose level, length, or flags aren't valid (requirement 6).
[[nodiscard]] std::string format_log_record(const LogClock& clock, const LogRecord& record);

/// The first line of every log file, without a newline (requirement 7):
/// "# <process> log; session started YYYY-MM-DD HH:MM:SS.ffffff ±HH:MM;
/// steady clock <s>.<nnnnnnnnn>".
[[nodiscard]] std::string format_log_header(std::string_view process, const LogClock& clock);

} // namespace ks::ipc
