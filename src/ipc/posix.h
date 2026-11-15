// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Private helpers for ks::ipc's system calls.

#include <fcntl.h>
#include <sys/types.h>

#include <cerrno>
#include <concepts>
#include <system_error>

namespace ks::ipc::detail {

/// The calling thread's errno, as an error code.
[[nodiscard]] inline std::error_code last_error() noexcept {
    return {errno, std::system_category()};
}

/// Calls `call` until it doesn't fail with EINTR, and returns its result.
template <std::invocable Call>
[[nodiscard]] auto retry_eintr(Call call) noexcept(noexcept(call())) {
    auto result = call();
    while (result == -1 && errno == EINTR) {
        result = call();
    }
    return result;
}

/// open() with a mode, retried on EINTR.
[[nodiscard]] inline int open_file(const char* path, int flags, mode_t mode) noexcept {
    return retry_eintr([&] {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open() is variadic
        return ::open(path, flags, mode);
    });
}

/// fcntl() with an int argument.
[[nodiscard]] inline int fcntl_int(int fd, int command, int argument) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl() is variadic
    return ::fcntl(fd, command, argument);
}

/// fcntl() with no argument.
[[nodiscard]] inline int fcntl_get(int fd, int command) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl() is variadic
    return ::fcntl(fd, command);
}

/// Sets FD_CLOEXEC on `fd`. Returns false, with errno set, if it fails.
[[nodiscard]] inline bool set_cloexec(int fd) noexcept {
    const int flags = fcntl_get(fd, F_GETFD);
    if (flags == -1) {
        return false;
    }
    return fcntl_int(fd, F_SETFD, flags | FD_CLOEXEC) != -1;
}

/// Sets O_NONBLOCK on `fd`. Returns false, with errno set, if it fails.
[[nodiscard]] inline bool set_nonblocking(int fd) noexcept {
    const int flags = fcntl_get(fd, F_GETFL);
    if (flags == -1) {
        return false;
    }
    return fcntl_int(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

/// True if `error` means a non-blocking call would have blocked.
[[nodiscard]] inline bool would_block(std::error_code error) noexcept {
    return error == std::errc::resource_unavailable_try_again ||
           error == std::errc::operation_would_block;
}

} // namespace ks::ipc::detail
