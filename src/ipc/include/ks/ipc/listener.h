// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The supervisor's socket, which external clients connect to
/// (specs/socket-protocol/spec.md requirements 15 to 19). Each end checks the
/// other's user ID.

#include <ks/ipc/fd.h>

#include <sys/types.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

namespace ks::ipc {

/// The supervisor's socket file, in the runtime directory.
inline constexpr std::string_view kSupervisorSocketName = "supervisor.sock";

/// `<runtime_dir>/supervisor.sock`.
[[nodiscard]] std::filesystem::path
supervisor_socket_path(const std::filesystem::path& runtime_dir);

/// The supervisor's listening socket. Move-only. Destroying it closes the
/// socket and unlinks the socket file, if the file is still its own.
class Listener {
public:
    /// Listens at supervisor_socket_path(runtime_dir), with a non-blocking,
    /// close-on-exec socket, and sets the file's mode to 0600. Replaces a
    /// socket file already at the path; refuses anything else there (EEXIST),
    /// and a path too long for sockaddr_un (ENAMETOOLONG).
    ///
    /// @pre The caller holds the supervisor's lock file, so no live supervisor
    /// owns a socket file this replaces.
    [[nodiscard]] static std::expected<Listener, std::error_code>
    open(const std::filesystem::path& runtime_dir);

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    Listener(Listener&& other) noexcept;
    Listener& operator=(Listener&& other) noexcept;
    ~Listener();

    /// The listening socket, for poll().
    [[nodiscard]] int socket() const noexcept { return socket_.get(); }

    /// Accepts one pending connection, and returns its socket: non-blocking,
    /// close-on-exec, and never raising SIGPIPE. Returns nothing if no
    /// connection is pending. A peer whose user ID isn't `expected_uid` is
    /// closed at once, and reported with std::errc::permission_denied.
    [[nodiscard]] std::expected<std::optional<Fd>, std::error_code> accept(uid_t expected_uid);

private:
    Listener(Fd socket, std::filesystem::path path, dev_t device, ino_t inode) noexcept;

    void close() noexcept;

    Fd socket_;
    std::filesystem::path path_; ///< Empty once moved from.
    dev_t device_ = 0;           ///< The socket file's identity, to unlink only our own.
    ino_t inode_ = 0;
};

/// Connects to the supervisor's socket in `runtime_dir`, and returns a
/// blocking, close-on-exec socket that never raises SIGPIPE. If the process
/// listening there doesn't have the user ID `expected_uid`, closes the socket
/// and returns std::errc::permission_denied. Refuses a path too long for
/// sockaddr_un (ENAMETOOLONG).
[[nodiscard]] std::expected<Fd, std::error_code>
connect_to_supervisor(const std::filesystem::path& runtime_dir, uid_t expected_uid);

/// The user ID of the process at the other end of a connection made through
/// the supervisor's socket: SO_PEERCRED on Linux, getpeereid() on macOS.
[[nodiscard]] std::expected<uid_t, std::error_code> peer_user_id(int socket);

} // namespace ks::ipc
