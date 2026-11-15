// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Passing file descriptors over Unix stream sockets (ADR 0003 §6,
/// specs/segments/spec.md requirements 16 and 17).
///
/// send_some() and receive_some() are one sendmsg() and one recvmsg() each,
/// so they work on blocking and non-blocking sockets alike. Framing whole
/// messages is the socket protocol's job.

#include <ks/ipc/fd.h>

#include <cstddef>
#include <expected>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace ks::ipc {

/// The most descriptors that travel with one send_some().
inline constexpr std::size_t kMaxFdsPerMessage = 8;

/// Two connected Unix stream sockets, both close-on-exec. Neither raises
/// SIGPIPE when written to after its peer has closed (macOS: SO_NOSIGPIPE;
/// Linux: send_some() uses MSG_NOSIGNAL).
[[nodiscard]] std::expected<std::pair<Fd, Fd>, std::error_code> socket_pair();

/// Sends as many of `bytes` as `socket` takes in one sendmsg(), with `fds`
/// attached, and returns how many bytes it sent. If it sent any, the
/// descriptors went with them. Writing to a closed peer returns EPIPE rather
/// than raising SIGPIPE. On a non-blocking socket that's full, it returns
/// EAGAIN, and nothing was sent.
///
/// Refuses, without sending anything (std::errc::invalid_argument), empty
/// `bytes` and more than kMaxFdsPerMessage descriptors.
[[nodiscard]] std::expected<std::size_t, std::error_code>
send_some(int socket, std::span<const std::byte> bytes, std::span<const int> fds);

/// Receives up to `buffer.size()` bytes from `socket` in one recvmsg(), and
/// returns how many it received: 0 means the peer closed the socket. Appends
/// any descriptors that came with those bytes to `fds`, each close-on-exec.
///
/// If more than kMaxFdsPerMessage descriptors came, or the control data was
/// truncated, it closes every descriptor that came with those bytes and
/// returns std::errc::message_size.
[[nodiscard]] std::expected<std::size_t, std::error_code>
receive_some(int socket, std::span<std::byte> buffer, std::vector<Fd>& fds);

} // namespace ks::ipc
