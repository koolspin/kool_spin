// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/supervisor/log_file.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

// Stubs (specs/logs/spec.md): nothing is created or written yet.

namespace ks::supervisor {

std::expected<LogFile, std::error_code> LogFile::create(const std::filesystem::path& path,
                                                        std::string_view process,
                                                        const ipc::LogClock& clock,
                                                        std::uint64_t limit) {
    static_cast<void>(path);
    static_cast<void>(process);
    static_cast<void>(clock);
    static_cast<void>(limit);
    return std::unexpected{std::make_error_code(std::errc::function_not_supported)};
}

LogFile::LogFile(ipc::Fd fd, const ipc::LogClock& clock, std::uint64_t limit,
                 std::uint64_t size) noexcept
    : fd_{std::move(fd)}, clock_{clock}, limit_{limit}, size_{size} {}

bool LogFile::write(std::uint64_t time, ipc::LogLevel level, std::string_view text) {
    static_cast<void>(time);
    static_cast<void>(level);
    static_cast<void>(text);
    static_cast<void>(fd_);
    static_cast<void>(clock_);
    static_cast<void>(limit_);
    static_cast<void>(size_);
    static_cast<void>(full_);
    return false;
}

bool LogFile::write(const ipc::LogRecord& record) {
    static_cast<void>(record);
    static_cast<void>(error_);
    return false;
}

std::optional<std::error_code> LogFile::error() const noexcept {
    return error_;
}

} // namespace ks::supervisor
