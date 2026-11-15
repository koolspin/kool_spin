// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// A connection: frames over a Unix stream socket, with descriptors attached
/// (specs/socket-protocol/spec.md requirements 11 to 14). It works on
/// blocking sockets, and on non-blocking ones in a poll() loop.

#include <ks/ipc/fd.h>
#include <ks/ipc/frame.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

namespace ks::ipc {

/// The most bytes of frames a connection queues before send() refuses more.
inline constexpr std::size_t kMaxQueuedBytes = std::size_t{1} << 20U;

/// The most descriptors a connection queues before send() refuses more.
inline constexpr std::size_t kMaxQueuedFds = 64;

/// A frame as received, with its descriptors, each close-on-exec.
struct ReceivedFrame {
    Frame frame;
    std::vector<Fd> fds;
};

/// What went wrong on a connection.
enum class ConnectionError : std::uint8_t {
    System,     ///< A system call failed: see ConnectionFailure::system.
    BadFrame,   ///< A frame was refused: see ConnectionFailure::frame.
    Closed,     ///< receive(): the peer closed the connection between frames.
    Truncated,  ///< receive(): the peer closed it in the middle of a frame.
    QueueFull,  ///< send(): the frame would queue more than kMaxQueuedBytes.
    TooManyFds, ///< send(): the frame would queue more than kMaxQueuedFds descriptors.
};

/// What a connection reports when something goes wrong.
struct ConnectionFailure {
    ConnectionError error = ConnectionError::System;
    FrameError frame = FrameError::BadVersion; ///< When `error` is BadFrame.
    std::error_code system;                    ///< When `error` is System.
};

/// Owns a connected Unix stream socket, blocking or non-blocking, with a
/// FrameReader and a queue of outgoing frames. Move-only.
class Connection {
public:
    explicit Connection(Fd socket) noexcept;

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;
    ~Connection();

    /// The socket, for poll().
    [[nodiscard]] int socket() const noexcept { return socket_.get(); }

    /// Queues a frame with close-on-exec duplicates of `fds`, and writes
    /// nothing: flush() does. The caller can close its own descriptors as soon
    /// as this returns. Refuses, queuing nothing: a frame encode_frame()
    /// refuses (BadFrame); a descriptor that can't be duplicated (System); a
    /// frame that would take the queue past kMaxQueuedBytes (QueueFull) or
    /// kMaxQueuedFds (TooManyFds).
    [[nodiscard]] std::expected<void, ConnectionFailure> send(std::uint16_t type,
                                                              std::uint32_t request_id,
                                                              std::span<const std::byte> body,
                                                              std::span<const int> fds);

    /// send() for a frame built by encode() (<ks/ipc/messages.h>). The frame's
    /// descriptor count must equal `fds.size()`.
    [[nodiscard]] std::expected<void, ConnectionFailure> send(const Frame& frame,
                                                              std::span<const int> fds);

    /// Writes queued frames in order. Each sendmsg() carries bytes of one
    /// frame only, and a frame's descriptors go with its first; they're closed
    /// once sent. Returns when the queue is empty, when a non-blocking socket
    /// is full (still success: see has_queued()), or on an error such as
    /// EPIPE.
    [[nodiscard]] std::expected<void, std::error_code> flush();

    /// True while frames are waiting to be written: poll() for POLLOUT.
    [[nodiscard]] bool has_queued() const noexcept { return !queue_.empty(); }

    /// Reads until a frame is complete, and returns it with its descriptors.
    /// Never reads past the frame it returns. On a non-blocking socket with no
    /// complete frame yet, returns nothing, keeping what it has read. End of
    /// file is Closed between frames and Truncated in the middle of one. After
    /// any error, every later call returns the same error.
    [[nodiscard]] std::expected<std::optional<ReceivedFrame>, ConnectionFailure> receive();

private:
    /// A queued frame: its bytes, how many of them have gone, and the
    /// duplicates of its descriptors until they go with its first bytes.
    struct Outgoing {
        std::vector<std::byte> bytes;
        std::size_t sent = 0;
        std::vector<Fd> fds;
    };

    Fd socket_;
    FrameReader reader_;
    std::vector<Fd> arrived_fds_; ///< Descriptors of the frame being read.
    std::deque<Outgoing> queue_;
    std::size_t queued_bytes_ = 0;
    std::size_t queued_fds_ = 0;
    std::optional<ConnectionFailure> failed_;
};

} // namespace ks::ipc
