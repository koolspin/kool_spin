// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/segments/spec.md: passing descriptors over Unix
// sockets.

#include <ks/ipc/fd_passing.h>

#include <ks/ipc/fd.h>

#include "segments_support.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <expected>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using ks::ipc::Fd;
using ks::ipc::kMaxFdsPerMessage;
using ks::ipc::receive_some;
using ks::ipc::send_some;
using ks::ipc::socket_pair;
using ks::ipc::test::is_cloexec;
using ks::ipc::test::open_fd_count;

/// The read ends of `count` new pipes, which are all different files. The
/// write ends are closed.
std::vector<Fd> pipe_read_ends(std::size_t count) {
    std::vector<Fd> ends;
    for (std::size_t i = 0; i < count; ++i) {
        std::array<int, 2> pipe_ends{-1, -1};
        if (::pipe(pipe_ends.data()) != 0) {
            return {};
        }
        ends.emplace_back(pipe_ends[0]);
        static_cast<void>(::close(pipe_ends[1]));
    }
    return ends;
}

/// The descriptors `fds` own.
std::vector<int> raw(const std::vector<Fd>& fds) {
    std::vector<int> result;
    result.reserve(fds.size());
    for (const Fd& fd : fds) {
        result.push_back(fd.get());
    }
    return result;
}

/// True if `a` and `b` refer to the same file.
bool same_file(int a, int b) {
    struct stat first{};
    struct stat second{};
    return ::fstat(a, &first) == 0 && ::fstat(b, &second) == 0 && first.st_dev == second.st_dev &&
           first.st_ino == second.st_ino;
}

/// Sends one byte with `fds` attached, with a plain sendmsg(), bypassing
/// send_some()'s limit. True if it went.
bool raw_send(int socket, const std::vector<int>& fds) {
    std::byte byte{0x42};
    iovec vector{.iov_base = &byte, .iov_len = 1};
    std::vector<std::byte> control(CMSG_SPACE(sizeof(int) * fds.size()));
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    // socklen_t on macOS, size_t on Linux.
    message.msg_controllen = static_cast<socklen_t>(control.size());
    cmsghdr* const header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = static_cast<socklen_t>(CMSG_LEN(sizeof(int) * fds.size()));
    std::memcpy(CMSG_DATA(header), fds.data(), sizeof(int) * fds.size());
    return ::sendmsg(socket, &message, 0) == 1;
}

} // namespace

TEST_CASE("Descriptors: travel with the bytes they were sent with", "[fd-passing]") {
    std::expected<std::pair<Fd, Fd>, std::error_code> sockets = socket_pair();
    REQUIRE(sockets.has_value());
    const std::vector<Fd> originals = pipe_read_ends(3);
    REQUIRE(originals.size() == 3);

    std::array<std::byte, 100> sent{};
    for (std::size_t i = 0; i < sent.size(); ++i) {
        sent.at(i) = static_cast<std::byte>(i + 1);
    }
    const std::vector<int> fds = raw(originals);
    const std::expected<std::size_t, std::error_code> count =
        send_some(sockets->first.get(), sent, fds);
    REQUIRE(count.has_value());
    REQUIRE(*count == sent.size());

    // Read it back 10 bytes at a time: the descriptors come with the first 10.
    std::array<std::byte, 100> received{};
    std::vector<Fd> got;
    for (std::size_t offset = 0; offset < received.size(); offset += 10) {
        CAPTURE(offset);
        const std::expected<std::size_t, std::error_code> part =
            receive_some(sockets->second.get(), std::span{received}.subspan(offset, 10), got);
        REQUIRE(part.has_value());
        REQUIRE(*part == 10);
        REQUIRE(got.size() == 3);
    }
    REQUIRE(received == sent);
    for (std::size_t i = 0; i < got.size(); ++i) {
        CAPTURE(i);
        REQUIRE(got.at(i).get() != originals.at(i).get());
        REQUIRE(same_file(got.at(i).get(), originals.at(i).get()));
        REQUIRE(is_cloexec(got.at(i).get()));
    }
}

TEST_CASE("Descriptors: too many are refused and closed", "[fd-passing]") {
    std::expected<std::pair<Fd, Fd>, std::error_code> sockets = socket_pair();
    REQUIRE(sockets.has_value());
    const std::vector<Fd> nine = pipe_read_ends(kMaxFdsPerMessage + 1);
    REQUIRE(nine.size() == kMaxFdsPerMessage + 1);
    const std::vector<int> fds = raw(nine);
    const std::array one{std::byte{0x01}};

    const std::expected<std::size_t, std::error_code> too_many =
        send_some(sockets->first.get(), one, fds);
    REQUIRE_FALSE(too_many.has_value());
    REQUIRE(too_many.error() == std::errc::invalid_argument);
    const std::expected<std::size_t, std::error_code> empty =
        send_some(sockets->first.get(), {}, {});
    REQUIRE_FALSE(empty.has_value());
    REQUIRE(empty.error() == std::errc::invalid_argument);

    // Neither refusal sent anything: the first byte to arrive is this one.
    const std::array marker{std::byte{0x7E}};
    REQUIRE(send_some(sockets->first.get(), marker, {}) == std::size_t{1});
    std::array<std::byte, 1> byte{};
    std::vector<Fd> got;
    REQUIRE(receive_some(sockets->second.get(), byte, got) == std::size_t{1});
    REQUIRE(byte == marker);
    REQUIRE(got.empty());

    // Nine descriptors sent around send_some() are refused, and closed.
    REQUIRE(raw_send(sockets->first.get(), fds));
    const int before = open_fd_count();
    const std::expected<std::size_t, std::error_code> refused =
        receive_some(sockets->second.get(), byte, got);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(got.empty());
    REQUIRE(open_fd_count() == before);
}

TEST_CASE("Descriptors: a closed peer", "[fd-passing]") {
    std::expected<std::pair<Fd, Fd>, std::error_code> sockets = socket_pair();
    REQUIRE(sockets.has_value());
    sockets->second.reset();

    // If this raised SIGPIPE, the test process would die here.
    const std::array one{std::byte{0x01}};
    const std::expected<std::size_t, std::error_code> sent =
        send_some(sockets->first.get(), one, {});
    REQUIRE_FALSE(sent.has_value());
    REQUIRE(sent.error() == std::errc::broken_pipe);

    std::array<std::byte, 16> buffer{};
    std::vector<Fd> got;
    REQUIRE(receive_some(sockets->first.get(), buffer, got) == std::size_t{0});
    REQUIRE(got.empty());
}
