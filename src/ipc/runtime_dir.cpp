// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/runtime_dir.h>

#include <ks/ipc/fd.h>

#include "posix.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace ks::ipc {

namespace {

using detail::last_error;
using detail::retry_eintr;

constexpr std::string_view kDirectoryName = "kool_spin";

/// `/tmp/kool_spin-<uid>`.
std::filesystem::path tmp_fallback() {
    return std::filesystem::path{"/tmp"} / std::format("{}-{}", kDirectoryName, ::geteuid());
}

/// True if `name` is a valid lock-file name (requirement 22).
bool is_lock_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxLockNameLength) {
        return false;
    }
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

std::filesystem::path lock_path(const std::filesystem::path& dir, std::string_view name) {
    return dir / (std::string{name} + ".lock");
}

/// The process ID in `text`, if it's a positive decimal number that fits
/// pid_t, followed by a newline or nothing (requirement 21).
std::optional<pid_t> parse_pid(std::string_view text) noexcept {
    if (text.ends_with('\n')) {
        text.remove_suffix(1);
    }
    if (text.empty() || !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    long long value = 0;
    const char* const first = std::to_address(text.begin());
    const char* const last = std::to_address(text.end());
    const auto [end, error] = std::from_chars(first, last, value);
    if (error != std::errc{} || end != last || value <= 0 ||
        value > std::numeric_limits<pid_t>::max()) {
        return std::nullopt;
    }
    return static_cast<pid_t>(value);
}

/// The process ID in the lock file `fd`, if it holds a valid one.
std::optional<pid_t> read_pid(int fd) noexcept {
    // The longest valid contents are 20 digits and a newline, so a full
    // buffer means something else.
    std::array<char, 32> contents{};
    const ssize_t read =
        retry_eintr([&] { return ::pread(fd, contents.data(), contents.size(), 0); });
    if (read <= 0 || static_cast<std::size_t>(read) >= contents.size()) {
        return std::nullopt;
    }
    return parse_pid(std::string_view{contents.data(), static_cast<std::size_t>(read)});
}

} // namespace

std::filesystem::path runtime_directory_path(std::optional<std::string_view> base) {
    if (base && base->starts_with('/')) {
        return std::filesystem::path{*base} / kDirectoryName;
    }
#ifdef __APPLE__
    std::array<char, 1024> temporary{};
    const std::size_t length =
        ::confstr(_CS_DARWIN_USER_TEMP_DIR, temporary.data(), temporary.size());
    if (length > 1 && length <= temporary.size()) {
        return std::filesystem::path{temporary.data()} / kDirectoryName;
    }
#endif
    return tmp_fallback();
}

std::filesystem::path default_runtime_directory_path() {
#ifdef __APPLE__
    constexpr const char* kVariable = "TMPDIR";
#else
    constexpr const char* kVariable = "XDG_RUNTIME_DIR";
#endif
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read at startup, before other threads exist
    const char* const value = std::getenv(kVariable);
    if (value == nullptr) {
        return runtime_directory_path(std::nullopt);
    }
    return runtime_directory_path(std::string_view{value});
}

std::expected<void, std::error_code> open_runtime_directory(const std::filesystem::path& path) {
    if (::mkdir(path.c_str(), 0700) == -1 && errno != EEXIST) {
        return std::unexpected{last_error()};
    }
    struct stat status{};
    if (::lstat(path.c_str(), &status) == -1) {
        return std::unexpected{last_error()};
    }
    if (!S_ISDIR(status.st_mode)) {
        return std::unexpected{std::make_error_code(std::errc::not_a_directory)};
    }
    if (status.st_uid != ::geteuid() || (status.st_mode & 0077U) != 0) {
        return std::unexpected{std::make_error_code(std::errc::permission_denied)};
    }
    return {};
}

std::expected<LockFile, LockFile::Failure> LockFile::acquire(const std::filesystem::path& dir,
                                                             std::string_view name) {
    if (!is_lock_name(name)) {
        return std::unexpected{
            Failure{.error = std::make_error_code(std::errc::invalid_argument), .holder = {}}};
    }
    const std::filesystem::path path = lock_path(dir, name);
    Fd fd{detail::open_file(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (!fd.valid()) {
        return std::unexpected{Failure{.error = last_error(), .holder = {}}};
    }
    int locked = -1;
    for (int attempt = 1;; ++attempt) {
        locked = retry_eintr([&] { return ::flock(fd.get(), LOCK_EX | LOCK_NB); });
        if (locked != -1 || errno != EWOULDBLOCK || attempt == kLockAttempts) {
            break;
        }
        std::this_thread::sleep_for(kLockRetry);
    }
    if (locked == -1) {
        if (errno == EWOULDBLOCK) {
            return std::unexpected{
                Failure{.error = std::make_error_code(std::errc::resource_unavailable_try_again),
                        .holder = read_pid(fd.get())}};
        }
        return std::unexpected{Failure{.error = last_error(), .holder = {}}};
    }
    const std::string contents = std::format("{}\n", ::getpid());
    if (retry_eintr([&] { return ::ftruncate(fd.get(), 0); }) == -1) {
        return std::unexpected{Failure{.error = last_error(), .holder = {}}};
    }
    const ssize_t written =
        retry_eintr([&] { return ::pwrite(fd.get(), contents.data(), contents.size(), 0); });
    if (written == -1) {
        return std::unexpected{Failure{.error = last_error(), .holder = {}}};
    }
    if (static_cast<std::size_t>(written) != contents.size()) {
        return std::unexpected{
            Failure{.error = std::make_error_code(std::errc::io_error), .holder = {}}};
    }
    return LockFile{std::move(fd)};
}

std::expected<LockState, std::error_code> probe_lock(const std::filesystem::path& dir,
                                                     std::string_view name) {
    if (!is_lock_name(name)) {
        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    }
    const std::filesystem::path path = lock_path(dir, name);
    const Fd fd{detail::open_file(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0)};
    if (!fd.valid()) {
        if (errno == ENOENT) {
            return LockState{.held = false, .pid = {}};
        }
        return std::unexpected{last_error()};
    }
    if (retry_eintr([&] { return ::flock(fd.get(), LOCK_SH | LOCK_NB); }) == 0) {
        static_cast<void>(::flock(fd.get(), LOCK_UN));
        return LockState{.held = false, .pid = {}};
    }
    if (errno == EWOULDBLOCK) {
        return LockState{.held = true, .pid = read_pid(fd.get())};
    }
    return std::unexpected{last_error()};
}

} // namespace ks::ipc
