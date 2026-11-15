// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/fd_passing.h>

#include <ks/ipc/fd.h>

#include "posix.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <expected>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace ks::ipc {

namespace {

using detail::last_error;
using detail::retry_eintr;

/// Room for twice the descriptors a message may carry, so that a few too
/// many arrive (and are closed) rather than being cut off by the kernel.
constexpr std::size_t kControlFds = 2 * kMaxFdsPerMessage;

/// Control data for up to `kControlFds` descriptors, aligned for cmsghdr.
struct ControlBuffer {
    alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(int) * kControlFds)> bytes{};
};

#ifdef __linux__
constexpr int kSendFlags = MSG_NOSIGNAL;
constexpr int kReceiveFlags = MSG_CMSG_CLOEXEC;
#else
constexpr int kSendFlags = 0;
constexpr int kReceiveFlags = 0;
#endif

/// Takes ownership of the descriptors in `message`'s control data.
std::vector<Fd> take_fds(msghdr& message) {
    std::vector<Fd> taken;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        const std::size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): control data is bytes
        const auto* const data = reinterpret_cast<const std::byte*>(CMSG_DATA(header));
        for (std::size_t i = 0; i < count; ++i) {
            int fd = -1;
            std::memcpy(&fd, std::span{data, count * sizeof(int)}.subspan(i * sizeof(int)).data(),
                        sizeof fd);
            taken.emplace_back(fd);
        }
    }
    return taken;
}

} // namespace

std::expected<std::pair<Fd, Fd>, std::error_code> socket_pair() {
    std::array<int, 2> fds{-1, -1};
#ifdef __linux__
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds.data()) == -1) {
        return std::unexpected{last_error()};
    }
    return std::pair{Fd{fds[0]}, Fd{fds[1]}};
#else
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == -1) {
        return std::unexpected{last_error()};
    }
    std::pair result{Fd{fds[0]}, Fd{fds[1]}};
    for (const int fd : fds) {
        constexpr int kOn = 1;
        if (!detail::set_cloexec(fd) ||
            ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &kOn, sizeof kOn) == -1) {
            return std::unexpected{last_error()};
        }
    }
    return result;
#endif
}

std::expected<std::size_t, std::error_code> send_some(int socket, std::span<const std::byte> bytes,
                                                      std::span<const int> fds) {
    if (bytes.empty() || fds.size() > kMaxFdsPerMessage) {
        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    }
    iovec vector{
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): sendmsg() only reads it
        .iov_base = const_cast<std::byte*>(bytes.data()),
        .iov_len = bytes.size(),
    };
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;

    ControlBuffer control;
    if (!fds.empty()) {
        message.msg_control = control.bytes.data();
        // socklen_t on macOS, size_t on Linux; the lengths here are a few
        // dozen bytes.
        message.msg_controllen = static_cast<socklen_t>(CMSG_SPACE(sizeof(int) * fds.size()));
        cmsghdr* const header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = static_cast<socklen_t>(CMSG_LEN(sizeof(int) * fds.size()));
        std::memcpy(CMSG_DATA(header), fds.data(), sizeof(int) * fds.size());
    }

    const ssize_t sent = retry_eintr([&] { return ::sendmsg(socket, &message, kSendFlags); });
    if (sent == -1) {
        return std::unexpected{last_error()};
    }
    return static_cast<std::size_t>(sent);
}

std::expected<std::size_t, std::error_code> receive_some(int socket, std::span<std::byte> buffer,
                                                         std::vector<Fd>& fds) {
    iovec vector{.iov_base = buffer.data(), .iov_len = buffer.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    ControlBuffer control;
    message.msg_control = control.bytes.data();
    message.msg_controllen = static_cast<socklen_t>(control.bytes.size());

    const ssize_t received =
        retry_eintr([&] { return ::recvmsg(socket, &message, kReceiveFlags); });
    if (received == -1) {
        return std::unexpected{last_error()};
    }

    // Owned from here on, so every early return closes them.
    std::vector<Fd> taken = take_fds(message);
    if ((message.msg_flags & MSG_CTRUNC) != 0 || taken.size() > kMaxFdsPerMessage) {
        return std::unexpected{std::make_error_code(std::errc::message_size)};
    }
#ifndef __linux__
    for (const Fd& fd : taken) {
        if (!detail::set_cloexec(fd.get())) {
            return std::unexpected{last_error()};
        }
    }
#endif
    for (Fd& fd : taken) {
        fds.push_back(std::move(fd));
    }
    return static_cast<std::size_t>(received);
}

} // namespace ks::ipc
