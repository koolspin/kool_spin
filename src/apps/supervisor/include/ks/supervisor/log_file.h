// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// One process's log file in a session folder (specs/logs/spec.md
/// requirement 8).

#include <ks/ipc/fd.h>
#include <ks/ipc/log.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace ks::supervisor {

/// The most bytes a log file holds, header included, before it drops lines.
inline constexpr std::uint64_t kLogFileLimit = std::uint64_t{16} * 1'024 * 1'024;

/// A log file that the supervisor writes. Each line goes out in one write()
/// call, unbuffered, so lines written before a crash are in the file.
class LogFile {
public:
    /// Creates `path`, failing if it exists, with mode 0600 and
    /// close-on-exec, and writes format_log_header(process, clock) as its
    /// first line. `clock` converts the time of every line written later.
    [[nodiscard]] static std::expected<LogFile, std::error_code>
    create(const std::filesystem::path& path, std::string_view process, const ipc::LogClock& clock,
           std::uint64_t limit = kLogFileLimit);

    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;
    LogFile(LogFile&&) noexcept = default;
    LogFile& operator=(LogFile&&) noexcept = default;
    ~LogFile() = default;

    /// Writes format_log_line(clock, time, level, text) and a newline.
    /// Returns whether the line was written.
    ///
    /// When a line would take the file past its limit, the file gets one
    /// last line instead, a warning that it has reached its limit, and every
    /// later line is dropped. After a write error, nothing more is written,
    /// and error() returns it.
    bool write(std::uint64_t time, ipc::LogLevel level, std::string_view text);

    /// Writes format_log_record(clock, record) and a newline, as the other
    /// write() does.
    bool write(const ipc::LogRecord& record);

    /// The write error that stopped the file, if one has.
    [[nodiscard]] std::optional<std::error_code> error() const noexcept;

private:
    LogFile(ipc::Fd fd, const ipc::LogClock& clock, std::uint64_t limit,
            std::uint64_t size) noexcept;

    /// Writes `line`, already formatted and without its newline, or the
    /// limit warning in its place, stamped `time`.
    bool write_formatted(std::uint64_t time, std::string line);

    ipc::Fd fd_;
    ipc::LogClock clock_;
    std::uint64_t limit_ = kLogFileLimit;
    std::uint64_t size_ = 0; ///< Bytes written so far, header included.
    bool full_ = false;
    std::optional<std::error_code> error_;
};

} // namespace ks::supervisor
