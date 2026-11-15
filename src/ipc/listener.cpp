// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/listener.h>

#include <ks/ipc/fd.h>

#include <sys/types.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

namespace ks::ipc {

// Stubs: specs/socket-protocol/spec.md isn't implemented yet.

namespace {

std::error_code not_implemented() {
    return std::make_error_code(std::errc::function_not_supported);
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
    path_.clear();
}

std::expected<Listener, std::error_code>
Listener::open(const std::filesystem::path& /*runtime_dir*/) {
    return std::unexpected{not_implemented()};
}

std::expected<std::optional<Fd>, std::error_code> Listener::accept(uid_t /*expected_uid*/) {
    close(); // open() never succeeds yet, so there's nothing to close.
    return std::unexpected{not_implemented()};
}

std::expected<Fd, std::error_code>
connect_to_supervisor(const std::filesystem::path& /*runtime_dir*/, uid_t /*expected_uid*/) {
    return std::unexpected{not_implemented()};
}

std::expected<uid_t, std::error_code> peer_user_id(int /*socket*/) {
    return std::unexpected{not_implemented()};
}

} // namespace ks::ipc
