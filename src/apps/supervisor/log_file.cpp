// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/supervisor/log_file.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ks::supervisor {

namespace {

/// The file's last line, once a line would take it past its limit.
constexpr std::string_view kLimitText =
    "this log file has reached its size limit; later lines are dropped";

struct WriteResult {
    std::uint64_t written = 0;            ///< Bytes that reached the file.
    std::optional<std::error_code> error; ///< Set if not all of them did.
};

/// Writes `line` in one write() call. Only a call that a signal interrupted,
/// or one that wrote part of the line, is followed by another, for the rest.
WriteResult write_line(int fd, std::string_view line) noexcept {
    WriteResult result;
    while (!line.empty()) {
        const ssize_t count = ::write(fd, line.data(), line.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            result.error = std::error_code{errno, std::generic_category()};
            return result;
        }
        if (count == 0) {
            // Nothing was written and nothing failed: don't spin on it.
            result.error = std::make_error_code(std::errc::io_error);
            return result;
        }
        const auto done = static_cast<std::size_t>(count);
        result.written += done;
        line.remove_prefix(done < line.size() ? done : line.size());
    }
    return result;
}

} // namespace

std::expected<LogFile, std::error_code> LogFile::create(const std::filesystem::path& path,
                                                        std::string_view process,
                                                        const ipc::LogClock& clock,
                                                        std::uint64_t limit) {
    constexpr mode_t kMode = 0600;
    constexpr int kFlags = O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC;
    ipc::Fd fd;
    while (!fd.valid()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open() is the POSIX interface.
        fd.reset(::open(path.c_str(), kFlags, kMode));
        if (!fd.valid() && errno != EINTR) {
            return std::unexpected{std::error_code{errno, std::generic_category()}};
        }
    }

    std::string header = ipc::format_log_header(process, clock);
    header.push_back('\n');
    const WriteResult result = write_line(fd.get(), header);
    if (result.error) {
        return std::unexpected{*result.error};
    }
    return LogFile{std::move(fd), clock, limit, result.written};
}

LogFile::LogFile(ipc::Fd fd, const ipc::LogClock& clock, std::uint64_t limit,
                 std::uint64_t size) noexcept
    : fd_{std::move(fd)}, clock_{clock}, limit_{limit}, size_{size} {}

bool LogFile::write(std::uint64_t time, ipc::LogLevel level, std::string_view text) {
    return write_formatted(time, ipc::format_log_line(clock_, time, level, text));
}

bool LogFile::write(const ipc::LogRecord& record) {
    return write_formatted(record.time, ipc::format_log_record(clock_, record));
}

bool LogFile::write_formatted(std::uint64_t time, std::string line) {
    if (full_ || error_) {
        return false;
    }
    line.push_back('\n');
    // size_ > limit_ only if the header alone passed the limit.
    const bool fits = size_ <= limit_ && line.size() <= limit_ - size_;
    if (!fits) {
        // The last line, with the time of the line it replaces. It may pass
        // the limit, by its own length at most.
        full_ = true;
        line = ipc::format_log_line(clock_, time, ipc::LogLevel::Warning, kLimitText);
        line.push_back('\n');
    }
    const WriteResult result = write_line(fd_.get(), line);
    size_ += result.written;
    if (result.error) {
        error_ = result.error;
        return false;
    }
    return fits;
}

std::optional<std::error_code> LogFile::error() const noexcept {
    return error_;
}

} // namespace ks::supervisor
