// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/child_process.h>

#include "posix.h"
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <algorithm>
#include <csignal>
#include <system_error>

namespace ks::ipc {

namespace {

/// kStopSignals, as a set.
sigset_t stop_signals() noexcept {
    sigset_t set{};
    static_cast<void>(sigemptyset(&set));
    for (const int signal : kStopSignals) {
        static_cast<void>(sigaddset(&set, signal));
    }
    return set;
}

/// True if `fd` is a stream socket.
bool is_stream_socket(int fd) noexcept {
    struct stat status{};
    if (::fstat(fd, &status) != 0 || !S_ISSOCK(status.st_mode)) {
        return false;
    }
    int type = 0;
    socklen_t size = sizeof type;
    return ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) == 0 && type == SOCK_STREAM;
}

} // namespace

std::error_code block_stop_signals() noexcept {
    const sigset_t set = stop_signals();
    return std::error_code{::pthread_sigmask(SIG_BLOCK, &set, nullptr), std::system_category()};
}

bool take_stop_signal() noexcept {
    sigset_t pending{};
    static_cast<void>(sigemptyset(&pending));
    if (::sigpending(&pending) != 0) {
        return false;
    }
    if (!std::ranges::any_of(kStopSignals,
                             [&](int signal) { return sigismember(&pending, signal) == 1; })) {
        return false;
    }
    // One is pending, and every thread blocks it, so this returns at once.
    const sigset_t set = stop_signals();
    int taken = 0;
    static_cast<void>(::sigwait(&set, &taken));
    return true;
}

bool adopt_socket(int fd) noexcept {
    return is_stream_socket(fd) && detail::set_cloexec(fd) && detail::set_nonblocking(fd);
}

bool adopt_descriptor(int fd) noexcept {
    return detail::fcntl_get(fd, F_GETFD) != -1 && detail::set_cloexec(fd);
}

} // namespace ks::ipc
