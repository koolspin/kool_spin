// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/listener.h>

#include <ks/ipc/fd.h>

#include "posix.h"
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>

namespace ks::ipc {

namespace {

using detail::last_error;
using detail::retry_eintr;
using detail::would_block;

/// The address of the Unix socket at `path`, or ENAMETOOLONG if it doesn't
/// fit with its final NUL (requirement 15).
std::expected<sockaddr_un, std::error_code> address_of(const std::filesystem::path& path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string& native = path.native();
    const std::span<char> destination{address.sun_path};
    if (native.size() + 1 > destination.size()) {
        return std::unexpected{std::make_error_code(std::errc::filename_too_long)};
    }
    std::ranges::copy(native, destination.begin());
    destination[native.size()] = '\0';
    return address;
}

/// `address` as the sockets API takes it.
const sockaddr* generic(const sockaddr_un& address) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
    return reinterpret_cast<const sockaddr*>(&address);
}

#ifndef __linux__
/// macOS: writing to `socket` after its peer has closed returns EPIPE rather
/// than raising SIGPIPE.
bool set_nosigpipe(int socket) noexcept {
    constexpr int kOn = 1;
    return ::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &kOn, sizeof kOn) == 0;
}
#endif

/// A new Unix stream socket, close-on-exec, that never raises SIGPIPE: on
/// Linux, send_some() passes MSG_NOSIGNAL.
std::expected<Fd, std::error_code> unix_socket() {
#ifdef __linux__
    Fd socket{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!socket.valid()) {
        return std::unexpected{last_error()};
    }
#else
    Fd socket{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (!socket.valid() || !detail::set_cloexec(socket.get()) || !set_nosigpipe(socket.get())) {
        return std::unexpected{last_error()};
    }
#endif
    return socket;
}

/// Refuses `socket` unless its peer has the user ID `expected_uid`.
std::expected<void, std::error_code> check_peer(int socket, uid_t expected_uid) {
    const std::expected<uid_t, std::error_code> uid = peer_user_id(socket);
    if (!uid) {
        return std::unexpected{uid.error()};
    }
    if (*uid != expected_uid) {
        return std::unexpected{std::make_error_code(std::errc::permission_denied)};
    }
    return {};
}

} // namespace

std::filesystem::path supervisor_socket_path(const std::filesystem::path& runtime_dir) {
    return runtime_dir / kSupervisorSocketName;
}

Listener::Listener(Fd socket, std::filesystem::path path, dev_t device, ino_t inode) noexcept
    : socket_{std::move(socket)}, path_{std::move(path)}, device_{device}, inode_{inode} {}

Listener::Listener(Listener&& other) noexcept
    : socket_{std::move(other.socket_)}, path_{std::exchange(other.path_, {})},
      device_{other.device_}, inode_{other.inode_} {}

Listener& Listener::operator=(Listener&& other) noexcept {
    if (this != &other) {
        close();
        socket_ = std::move(other.socket_);
        path_ = std::exchange(other.path_, {});
        device_ = other.device_;
        inode_ = other.inode_;
    }
    return *this;
}

Listener::~Listener() {
    close();
}

void Listener::close() noexcept {
    socket_.reset();
    if (path_.empty()) {
        return;
    }
    // Unlink the socket file only if it's still the one this listener bound.
    struct stat status{};
    if (::lstat(path_.c_str(), &status) == 0 && status.st_dev == device_ &&
        status.st_ino == inode_) {
        static_cast<void>(::unlink(path_.c_str()));
    }
    path_.clear();
}

std::expected<Listener, std::error_code> Listener::open(const std::filesystem::path& runtime_dir) {
    std::filesystem::path path = supervisor_socket_path(runtime_dir);
    const std::expected<sockaddr_un, std::error_code> address = address_of(path);
    if (!address) {
        return std::unexpected{address.error()};
    }

    // A socket file here was left by a supervisor that died: the caller holds
    // the supervisor's lock. Anything else isn't ours to remove.
    struct stat existing{};
    if (::lstat(path.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode)) {
            return std::unexpected{std::make_error_code(std::errc::file_exists)};
        }
        if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
            return std::unexpected{last_error()};
        }
    } else if (errno != ENOENT) {
        return std::unexpected{last_error()};
    }

    std::expected<Fd, std::error_code> socket = unix_socket();
    if (!socket) {
        return std::unexpected{socket.error()};
    }
    if (::bind(socket->get(), generic(*address), sizeof *address) != 0) {
        return std::unexpected{last_error()};
    }
    struct stat bound{};
    if (::lstat(path.c_str(), &bound) != 0) {
        const std::error_code error = last_error();
        static_cast<void>(::unlink(path.c_str()));
        return std::unexpected{error};
    }
    // From here on, the Listener unlinks the file if anything fails.
    Listener listener{std::move(*socket), std::move(path), bound.st_dev, bound.st_ino};
    if (::chmod(listener.path_.c_str(), 0600) != 0 ||
        ::listen(listener.socket_.get(), SOMAXCONN) != 0 ||
        !detail::set_nonblocking(listener.socket_.get())) {
        return std::unexpected{last_error()};
    }
    return listener;
}

std::expected<std::optional<Fd>, std::error_code> Listener::accept(uid_t expected_uid) {
#ifdef __linux__
    Fd accepted{retry_eintr(
        [&] { return ::accept4(socket_.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK); })};
#else
    Fd accepted{retry_eintr([&] { return ::accept(socket_.get(), nullptr, nullptr); })};
#endif
    if (!accepted.valid()) {
        const std::error_code error = last_error();
        if (would_block(error)) {
            return std::optional<Fd>{};
        }
        return std::unexpected{error};
    }
#ifndef __linux__
    if (!detail::set_cloexec(accepted.get()) || !detail::set_nonblocking(accepted.get()) ||
        !set_nosigpipe(accepted.get())) {
        return std::unexpected{last_error()};
    }
#endif
    // A refused peer is closed as `accepted` goes out of scope.
    if (const std::expected<void, std::error_code> checked =
            check_peer(accepted.get(), expected_uid);
        !checked) {
        return std::unexpected{checked.error()};
    }
    return std::optional<Fd>{std::move(accepted)};
}

std::expected<Fd, std::error_code> connect_to_supervisor(const std::filesystem::path& runtime_dir,
                                                         uid_t expected_uid) {
    const std::expected<sockaddr_un, std::error_code> address =
        address_of(supervisor_socket_path(runtime_dir));
    if (!address) {
        return std::unexpected{address.error()};
    }
    std::expected<Fd, std::error_code> socket = unix_socket();
    if (!socket) {
        return std::unexpected{socket.error()};
    }
    if (retry_eintr([&] { return ::connect(socket->get(), generic(*address), sizeof *address); }) !=
        0) {
        return std::unexpected{last_error()};
    }
    if (const std::expected<void, std::error_code> checked =
            check_peer(socket->get(), expected_uid);
        !checked) {
        return std::unexpected{checked.error()};
    }
    return std::move(*socket);
}

std::expected<uid_t, std::error_code> peer_user_id(int socket) {
#ifdef __linux__
    ucred credentials{};
    socklen_t size = sizeof credentials;
    if (::getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0) {
        return std::unexpected{last_error()};
    }
    return credentials.uid;
#else
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(socket, &uid, &gid) != 0) {
        return std::unexpected{last_error()};
    }
    return uid;
#endif
}

} // namespace ks::ipc
