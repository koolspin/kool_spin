// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/log.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Stubs (specs/logs/spec.md): every function does nothing yet.

namespace ks::ipc {

LogWriter::LogWriter(LogSegment& segment) noexcept : segment_{&segment}, producer_{segment.ring} {}

void LogWriter::write(LogLevel level, std::string_view text) {
    static_cast<void>(segment_);
    static_cast<void>(level);
    static_cast<void>(text);
}

std::uint64_t LogWriter::dropped() const {
    const std::scoped_lock lock{mutex_};
    return dropped_;
}

LogReader::LogReader(LogSegment& segment) noexcept : segment_{&segment}, consumer_{segment.ring} {}

std::optional<LogRecord> LogReader::next() noexcept {
    static_cast<void>(consumer_);
    return std::nullopt;
}

std::uint64_t LogReader::take_dropped() noexcept {
    static_cast<void>(segment_);
    return dropped_;
}

bool LogReader::corrupt() const noexcept {
    return dropped_corrupt_;
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
