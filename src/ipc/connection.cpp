// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/connection.h>

#include <ks/ipc/fd.h>
#include <ks/ipc/fd_passing.h>
#include <ks/ipc/frame.h>

#include "posix.h"
#include <fcntl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace ks::ipc {

namespace {

using detail::last_error;
using detail::would_block;

/// A failure with no frame error or system error to report.
ConnectionFailure failure(ConnectionError error) {
    ConnectionFailure result;
    result.error = error;
    return result;
}

ConnectionFailure system_failure(std::error_code error) {
    ConnectionFailure result = failure(ConnectionError::System);
    result.system = error;
    return result;
}

ConnectionFailure frame_failure(FrameError error) {
    ConnectionFailure result = failure(ConnectionError::BadFrame);
    result.frame = error;
    return result;
}

} // namespace

Connection::Connection(Fd socket) noexcept : socket_{std::move(socket)} {}

Connection::Connection(Connection&&) noexcept = default;
Connection& Connection::operator=(Connection&&) noexcept = default;
Connection::~Connection() = default;

std::expected<void, ConnectionFailure> Connection::send(std::uint16_t type,
                                                        std::uint32_t request_id,
                                                        std::span<const std::byte> body,
                                                        std::span<const int> fds) {
    std::expected<std::vector<std::byte>, FrameError> bytes =
        encode_frame(type, request_id, body, fds.size());
    if (!bytes) {
        return std::unexpected{frame_failure(bytes.error())};
    }
    if (bytes->size() > kMaxQueuedBytes - queued_bytes_) {
        return std::unexpected{failure(ConnectionError::QueueFull)};
    }
    if (fds.size() > kMaxQueuedFds - queued_fds_) {
        return std::unexpected{failure(ConnectionError::TooManyFds)};
    }

    Outgoing frame{.bytes = std::move(*bytes), .sent = 0, .fds = {}};
    frame.fds.reserve(fds.size());
    for (const int fd : fds) {
        Fd duplicate{detail::fcntl_int(fd, F_DUPFD_CLOEXEC, 0)};
        if (!duplicate.valid()) {
            return std::unexpected{system_failure(last_error())};
        }
        frame.fds.push_back(std::move(duplicate));
    }
    queued_bytes_ += frame.bytes.size();
    queued_fds_ += frame.fds.size();
    queue_.push_back(std::move(frame));
    return {};
}

std::expected<void, ConnectionFailure> Connection::send(const Frame& frame,
                                                        std::span<const int> fds) {
    if (frame.descriptor_count != fds.size()) {
        return std::unexpected{frame_failure(FrameError::FdMismatch)};
    }
    return send(frame.type, frame.request_id, frame.body, fds);
}

std::expected<void, std::error_code> Connection::flush() {
    while (!queue_.empty()) {
        Outgoing& front = queue_.front();
        // The descriptors go with the frame's first bytes, and only those.
        std::array<int, kMaxFdsPerMessage> fds{};
        std::size_t count = 0;
        if (front.sent == 0) {
            for (const Fd& fd : front.fds) {
                std::span{fds}[count] = fd.get();
                ++count;
            }
        }
        const std::expected<std::size_t, std::error_code> sent = send_some(
            socket_.get(), std::span{front.bytes}.subspan(front.sent), std::span{fds}.first(count));
        if (!sent) {
            if (would_block(sent.error())) {
                return {};
            }
            return std::unexpected{sent.error()};
        }
        if (count > 0) {
            queued_fds_ -= front.fds.size();
            front.fds.clear();
        }
        front.sent += *sent;
        if (front.sent == front.bytes.size()) {
            queued_bytes_ -= front.bytes.size();
            queue_.pop_front();
        }
    }
    return {};
}

std::expected<std::optional<ReceivedFrame>, ConnectionFailure> Connection::receive() {
    if (failed_) {
        return std::unexpected{*failed_};
    }
    const auto fail = [this](ConnectionFailure failure) {
        // The descriptors of a frame that never completes are closed.
        arrived_fds_.clear();
        failed_ = failure;
        return std::unexpected{failure};
    };

    for (;;) {
        // Never past the frame being read (requirement 6).
        const std::span<std::byte> buffer = reader_.next();
        const std::size_t had = arrived_fds_.size();
        const std::expected<std::size_t, std::error_code> received =
            receive_some(socket_.get(), buffer, arrived_fds_);
        if (!received) {
            if (would_block(received.error())) {
                return std::optional<ReceivedFrame>{};
            }
            return fail(system_failure(received.error()));
        }
        if (*received == 0) {
            return fail(failure(reader_.at_boundary() ? ConnectionError::Closed
                                                      : ConnectionError::Truncated));
        }
        std::expected<std::optional<Frame>, FrameError> committed =
            reader_.commit(*received, arrived_fds_.size() - had);
        if (!committed) {
            return fail(frame_failure(committed.error()));
        }
        std::optional<Frame>& frame = *committed;
        if (frame) {
            ReceivedFrame complete{.frame = std::move(*frame), .fds = std::move(arrived_fds_)};
            arrived_fds_.clear();
            return complete;
        }
    }
}

} // namespace ks::ipc
