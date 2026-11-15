// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/socket-protocol/spec.md: connections.

#include <ks/ipc/connection.h>

#include <ks/ipc/fd.h>
#include <ks/ipc/fd_passing.h>
#include <ks/ipc/frame.h>

#include "protocol_support.h"
#include "segments_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using ks::ipc::Connection;
using ks::ipc::ConnectionError;
using ks::ipc::ConnectionFailure;
using ks::ipc::encode_frame;
using ks::ipc::Fd;
using ks::ipc::FrameError;
using ks::ipc::kFrameHeaderSize;
using ks::ipc::kMaxBodySize;
using ks::ipc::kMaxFdsPerMessage;
using ks::ipc::kMaxQueuedBytes;
using ks::ipc::kMaxQueuedFds;
using ks::ipc::ReceivedFrame;
using ks::ipc::send_some;
using ks::ipc::socket_pair;
using ks::ipc::test::bytes;
using ks::ipc::test::Exchanged;
using ks::ipc::test::file_id;
using ks::ipc::test::FileId;
using ks::ipc::test::is_cloexec;
using ks::ipc::test::open_fd_count;
using ks::ipc::test::open_files;
using ks::ipc::test::pattern;
using ks::ipc::test::raw;
using ks::ipc::test::set_buffer_sizes;
using ks::ipc::test::set_nonblocking;
using ks::ipc::test::TempDir;

/// Two connected Connections over a socket_pair(). If it fails, neither has a
/// socket: see connected().
std::pair<Connection, Connection> connected_pair() {
    std::expected<std::pair<Fd, Fd>, std::error_code> sockets = socket_pair();
    if (!sockets) {
        return std::pair{Connection{Fd{}}, Connection{Fd{}}};
    }
    return std::pair{Connection{std::move(sockets->first)}, Connection{std::move(sockets->second)}};
}

/// True if both of `pair` have sockets.
bool connected(const std::pair<Connection, Connection>& pair) {
    return pair.first.socket() >= 0 && pair.second.socket() >= 0;
}

/// The identities of the files `fds` refer to.
std::vector<FileId> ids_of(std::span<const Fd> fds) {
    std::vector<FileId> ids;
    for (const Fd& fd : fds) {
        ids.push_back(file_id(fd.get()).value_or(FileId{}));
    }
    return ids;
}

/// True if `received` failed with a refused frame, for `reason`.
bool refused_for(const std::expected<std::optional<ReceivedFrame>, ConnectionFailure>& received,
                 FrameError reason) {
    return !received && received.error().error == ConnectionError::BadFrame &&
           received.error().frame == reason;
}

/// True if `received` failed with `error`.
bool failed_with(const std::expected<std::optional<ReceivedFrame>, ConnectionFailure>& received,
                 ConnectionError error) {
    return !received && received.error().error == error;
}

} // namespace

TEST_CASE("Connections: descriptors stay with their frames", "[connections]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());
    std::pair<Connection, Connection> pair = connected_pair();
    REQUIRE(connected(pair));
    auto& [sender, receiver] = pair;

    std::vector<Fd> files = open_files(dir.path(), 5);
    REQUIRE(files.size() == 5);
    const std::vector<FileId> ids = ids_of(files);
    const std::vector<int> fds = raw(files);
    const std::span<const int> all{fds};

    REQUIRE(sender.send(1, 11, bytes({0x01}), all.subspan(0, 2)));
    REQUIRE(sender.send(2, 12, bytes({0x02, 0x02}), {}));
    REQUIRE(sender.send(3, 13, bytes({}), all.subspan(2, 3)));
    // The sender's own copies can go as soon as send() returns.
    files.clear();
    REQUIRE(sender.flush());
    REQUIRE_FALSE(sender.has_queued());

    const std::array<std::size_t, 3> counts{2, 0, 3};
    const std::array<std::size_t, 3> first_file{0, 2, 2};
    for (std::size_t i = 0; i < counts.size(); ++i) {
        CAPTURE(i);
        std::expected<std::optional<ReceivedFrame>, ConnectionFailure> received =
            receiver.receive();
        REQUIRE(received.has_value());
        REQUIRE(received->has_value());
        const ReceivedFrame frame = std::move(*received).value_or(ReceivedFrame{});
        REQUIRE(frame.frame.type == i + 1);
        REQUIRE(frame.frame.request_id == i + 11);
        REQUIRE(frame.frame.body.size() == (i == 2 ? 0 : i + 1));
        REQUIRE(frame.frame.descriptor_count == counts.at(i));
        REQUIRE(frame.fds.size() == counts.at(i));
        for (std::size_t j = 0; j < frame.fds.size(); ++j) {
            CAPTURE(j);
            REQUIRE(file_id(frame.fds.at(j).get()) == ids.at(first_file.at(i) + j));
            REQUIRE(is_cloexec(frame.fds.at(j).get()));
        }
    }
}

TEST_CASE("Connections: big frames through small buffers", "[connections]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());
    std::pair<Connection, Connection> pair = connected_pair();
    REQUIRE(connected(pair));
    auto& [sender, receiver] = pair;
    for (const int socket : {sender.socket(), receiver.socket()}) {
        REQUIRE(set_nonblocking(socket));
        REQUIRE(set_buffer_sizes(socket, 4096));
    }

    // Nothing has arrived yet: "nothing yet", not an error.
    const std::expected<std::optional<ReceivedFrame>, ConnectionFailure> nothing =
        receiver.receive();
    REQUIRE(nothing.has_value());
    REQUIRE_FALSE(nothing->has_value());

    const std::vector<Fd> files = open_files(dir.path(), 3);
    REQUIRE(files.size() == 3);
    const std::vector<FileId> ids = ids_of(files);
    const std::vector<std::byte> big = pattern(kMaxBodySize, 7);
    REQUIRE(sender.send(100, 1, big, raw(files)));
    constexpr std::size_t kSmall = 100;
    for (std::size_t i = 0; i < kSmall; ++i) {
        REQUIRE(sender.send(101, static_cast<std::uint32_t>(i + 2), pattern(i, 1), {}));
    }

    const Exchanged exchanged = ks::ipc::test::exchange(sender, receiver, kSmall + 1);
    REQUIRE_FALSE(exchanged.failed);
    REQUIRE(exchanged.saw_queued);
    REQUIRE(exchanged.frames.size() == kSmall + 1);
    REQUIRE_FALSE(sender.has_queued());

    const ReceivedFrame& first = exchanged.frames.front();
    REQUIRE(first.frame.type == 100);
    REQUIRE(first.frame.request_id == 1);
    REQUIRE(first.frame.body == big);
    REQUIRE(ids_of(first.fds) == ids);
    for (std::size_t i = 0; i < kSmall; ++i) {
        CAPTURE(i);
        const ReceivedFrame& frame = exchanged.frames.at(i + 1);
        REQUIRE(frame.frame.type == 101);
        REQUIRE(frame.frame.request_id == i + 2);
        REQUIRE(frame.frame.body == pattern(i, 1));
        REQUIRE(frame.fds.empty());
    }
}

TEST_CASE("Connections: a descriptor count that doesn't match", "[connections]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());
    const std::vector<Fd> files = open_files(dir.path(), 2);
    REQUIRE(files.size() == 2);

    SECTION("one declared, none attached") {
        std::expected<std::pair<Fd, Fd>, std::error_code> sockets = socket_pair();
        REQUIRE(sockets.has_value());
        const std::vector<std::byte> frame =
            encode_frame(5, 0, bytes({1, 2, 3}), 1).value_or(std::vector<std::byte>{});
        REQUIRE(send_some(sockets->first.get(), frame, {}) == frame.size());
        Connection receiver{std::move(sockets->second)};

        REQUIRE(refused_for(receiver.receive(), FrameError::FdMismatch));
        REQUIRE(refused_for(receiver.receive(), FrameError::FdMismatch));
    }

    SECTION("none declared, two attached") {
        std::expected<std::pair<Fd, Fd>, std::error_code> sockets = socket_pair();
        REQUIRE(sockets.has_value());
        const std::vector<std::byte> frame =
            encode_frame(5, 0, bytes({1, 2, 3}), 0).value_or(std::vector<std::byte>{});
        REQUIRE(send_some(sockets->first.get(), frame, raw(files)) == frame.size());
        Connection receiver{std::move(sockets->second)};

        const int before = open_fd_count();
        REQUIRE(refused_for(receiver.receive(), FrameError::FdMismatch));
        REQUIRE(open_fd_count() == before);
        REQUIRE(refused_for(receiver.receive(), FrameError::FdMismatch));
    }
}

TEST_CASE("Connections: closed by the peer", "[connections]") {
    SECTION("between frames") {
        std::pair<Connection, Connection> pair = connected_pair();
        REQUIRE(connected(pair));
        {
            Connection sender = std::move(pair.first);
            REQUIRE(sender.send(1, 1, bytes({1}), {}));
            REQUIRE(sender.flush());
        }
        Connection& receiver = pair.second;
        const std::expected<std::optional<ReceivedFrame>, ConnectionFailure> frame =
            receiver.receive();
        REQUIRE(frame.has_value());
        REQUIRE(frame->has_value());
        REQUIRE(failed_with(receiver.receive(), ConnectionError::Closed));
        REQUIRE(failed_with(receiver.receive(), ConnectionError::Closed));
    }

    SECTION("in the middle of a frame") {
        const std::vector<std::byte> frame =
            encode_frame(1, 1, pattern(100, 0), 0).value_or(std::vector<std::byte>{});
        REQUIRE(frame.size() == kFrameHeaderSize + 100);
        // Cut short in the header, and in the body.
        for (const std::size_t cut : {std::size_t{10}, kFrameHeaderSize + 50}) {
            CAPTURE(cut);
            std::expected<std::pair<Fd, Fd>, std::error_code> sockets = socket_pair();
            REQUIRE(sockets.has_value());
            REQUIRE(send_some(sockets->first.get(), std::span{frame}.first(cut), {}) == cut);
            sockets->first.reset();
            Connection receiver{std::move(sockets->second)};

            REQUIRE(failed_with(receiver.receive(), ConnectionError::Truncated));
            REQUIRE(failed_with(receiver.receive(), ConnectionError::Truncated));
        }
    }

    SECTION("writing to a closed peer") {
        std::pair<Connection, Connection> pair = connected_pair();
        REQUIRE(connected(pair));
        Connection& sender = pair.first;
        {
            const Connection closed = std::move(pair.second);
        }

        // If this raised SIGPIPE, the test process would die here.
        REQUIRE(sender.send(1, 1, bytes({1}), {}));
        const std::expected<void, std::error_code> flushed = sender.flush();
        REQUIRE_FALSE(flushed.has_value());
        REQUIRE(flushed.error() == std::errc::broken_pipe);
    }
}

TEST_CASE("Connections: the outgoing queue is bounded", "[connections]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());

    SECTION("bytes") {
        std::pair<Connection, Connection> pair = connected_pair();
        REQUIRE(connected(pair));
        auto& [sender, receiver] = pair;
        for (const int socket : {sender.socket(), receiver.socket()}) {
            REQUIRE(set_nonblocking(socket));
        }

        // Nothing is flushed, so every frame stays queued.
        const std::vector<std::byte> body = pattern(kMaxBodySize, 0);
        constexpr std::size_t kFits = kMaxQueuedBytes / (kFrameHeaderSize + kMaxBodySize);
        for (std::size_t i = 0; i < kFits; ++i) {
            CAPTURE(i);
            REQUIRE(sender.send(1, static_cast<std::uint32_t>(i), body, {}));
        }
        const std::expected<void, ConnectionFailure> full = sender.send(1, 999, body, {});
        REQUIRE_FALSE(full.has_value());
        REQUIRE(full.error().error == ConnectionError::QueueFull);
        // A frame that still fits is accepted.
        REQUIRE(sender.send(2, 1000, {}, {}));

        const Exchanged exchanged = ks::ipc::test::exchange(sender, receiver, kFits + 1);
        REQUIRE_FALSE(exchanged.failed);
        REQUIRE(exchanged.frames.size() == kFits + 1);
        for (std::size_t i = 0; i < kFits; ++i) {
            CAPTURE(i);
            REQUIRE(exchanged.frames.at(i).frame.request_id == i);
            REQUIRE(exchanged.frames.at(i).frame.body == body);
        }
        REQUIRE(exchanged.frames.back().frame.request_id == 1000);
    }

    SECTION("descriptors") {
        std::pair<Connection, Connection> pair = connected_pair();
        REQUIRE(connected(pair));
        auto& [sender, receiver] = pair;
        for (const int socket : {sender.socket(), receiver.socket()}) {
            REQUIRE(set_nonblocking(socket));
        }
        const std::vector<Fd> files = open_files(dir.path(), kMaxFdsPerMessage);
        REQUIRE(files.size() == kMaxFdsPerMessage);
        const std::vector<int> eight = raw(files);

        constexpr std::size_t kFits = kMaxQueuedFds / kMaxFdsPerMessage;
        for (std::size_t i = 0; i < kFits; ++i) {
            CAPTURE(i);
            REQUIRE(sender.send(1, static_cast<std::uint32_t>(i), {}, eight));
        }
        const std::expected<void, ConnectionFailure> full =
            sender.send(1, 999, {}, std::span{eight}.first(1));
        REQUIRE_FALSE(full.has_value());
        REQUIRE(full.error().error == ConnectionError::TooManyFds);
        // A frame without descriptors is accepted.
        REQUIRE(sender.send(2, 1000, bytes({1}), {}));

        const Exchanged exchanged = ks::ipc::test::exchange(sender, receiver, kFits + 1);
        REQUIRE_FALSE(exchanged.failed);
        REQUIRE(exchanged.frames.size() == kFits + 1);
        for (std::size_t i = 0; i < kFits; ++i) {
            CAPTURE(i);
            REQUIRE(exchanged.frames.at(i).frame.request_id == i);
            REQUIRE(exchanged.frames.at(i).fds.size() == kMaxFdsPerMessage);
        }
        REQUIRE(exchanged.frames.back().frame.request_id == 1000);
        REQUIRE(exchanged.frames.back().fds.empty());
    }
}
