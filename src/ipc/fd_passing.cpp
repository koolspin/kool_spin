// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/fd_passing.h>

#include <ks/ipc/fd.h>

#include <cstddef>
#include <expected>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace ks::ipc {

// Stubs, until the maintainer approves the acceptance tests for
// specs/segments/spec.md.

std::expected<std::pair<Fd, Fd>, std::error_code> socket_pair() {
    return std::unexpected{std::make_error_code(std::errc::function_not_supported)};
}

std::expected<std::size_t, std::error_code>
send_some(int /*socket*/, std::span<const std::byte> /*bytes*/, std::span<const int> /*fds*/) {
    return std::unexpected{std::make_error_code(std::errc::function_not_supported)};
}

std::expected<std::size_t, std::error_code>
receive_some(int /*socket*/, std::span<std::byte> /*buffer*/, std::vector<Fd>& /*fds*/) {
    return std::unexpected{std::make_error_code(std::errc::function_not_supported)};
}

} // namespace ks::ipc
