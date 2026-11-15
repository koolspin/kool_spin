// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/connection.h>

#include <ks/ipc/fd.h>
#include <ks/ipc/frame.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

namespace ks::ipc {

// Stubs: specs/socket-protocol/spec.md isn't implemented yet.

namespace {

ConnectionFailure not_implemented() {
    return {.error = ConnectionError::System,
            .system = std::make_error_code(std::errc::function_not_supported)};
}

} // namespace

Connection::Connection(Fd socket) noexcept : socket_{std::move(socket)} {}

Connection::Connection(Connection&&) noexcept = default;
Connection& Connection::operator=(Connection&&) noexcept = default;
Connection::~Connection() = default;

std::expected<void, ConnectionFailure> Connection::send(std::uint16_t /*type*/,
                                                        std::uint32_t /*request_id*/,
                                                        std::span<const std::byte> /*body*/,
                                                        std::span<const int> /*fds*/) {
    failed_ = not_implemented();
    return std::unexpected{not_implemented()};
}

std::expected<void, ConnectionFailure> Connection::send(const Frame& /*frame*/,
                                                        std::span<const int> /*fds*/) {
    failed_ = not_implemented();
    return std::unexpected{not_implemented()};
}

std::expected<void, std::error_code> Connection::flush() {
    queued_bytes_ = 0;
    queued_fds_ = 0;
    return std::unexpected{std::make_error_code(std::errc::function_not_supported)};
}

std::expected<std::optional<ReceivedFrame>, ConnectionFailure> Connection::receive() {
    failed_ = not_implemented();
    return std::unexpected{not_implemented()};
}

} // namespace ks::ipc
