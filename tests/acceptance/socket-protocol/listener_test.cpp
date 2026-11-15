// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/socket-protocol/spec.md: the supervisor's socket.

#include <ks/ipc/listener.h>

#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/messages.h>

#include "protocol_support.h"
#include "segments_support.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace {

using ks::ipc::connect_to_supervisor;
using ks::ipc::Connection;
using ks::ipc::ConnectionError;
using ks::ipc::ConnectionFailure;
using ks::ipc::decode;
using ks::ipc::encode;
using ks::ipc::Fd;
using ks::ipc::Frame;
using ks::ipc::Listener;
using ks::ipc::Message;
using ks::ipc::peer_user_id;
using ks::ipc::ReceivedFrame;
using ks::ipc::supervisor_socket_path;
using ks::ipc::test::leave_socket_file;
using ks::ipc::test::mode_of;
using ks::ipc::test::receive_within;
using ks::ipc::test::TempDir;
using ks::ipc::test::wait_readable;

/// Waits up to 5 seconds for a connection, and accepts it.
std::expected<std::optional<Fd>, std::error_code> accept_within(Listener& listener,
                                                                uid_t expected_uid) {
    static_cast<void>(wait_readable(listener.socket(), 5000));
    return listener.accept(expected_uid);
}

/// Sends `message` on `connection` and flushes it. False if either fails.
bool send_message(Connection& connection, const Message& message) {
    const std::expected<Frame, ks::ipc::MessageError> frame = encode(message);
    return frame && connection.send(*frame, {}) && connection.flush();
}

/// Receives a message on `connection`, waiting up to 5 seconds. Nothing if
/// none arrives, or it isn't a valid message.
std::optional<Message> receive_message(Connection& connection) {
    std::expected<std::optional<ReceivedFrame>, ConnectionFailure> received =
        receive_within(connection);
    if (!received || !*received) {
        return std::nullopt;
    }
    std::expected<Message, ks::ipc::MessageError> message = decode((*received)->frame);
    if (!message) {
        return std::nullopt;
    }
    return std::move(*message);
}

/// `asker` sends a Ping with `request_id`; `answerer` receives it and sends a
/// Pong with the same ID; `asker` receives that. True if all of it worked.
bool ping_pong(Connection& asker, Connection& answerer, std::uint32_t request_id) {
    if (!send_message(asker, Message{.request_id = request_id, .body = ks::ipc::msg::Ping{}})) {
        return false;
    }
    const std::optional<Message> ping = receive_message(answerer);
    if (!ping || ping->request_id != request_id ||
        !std::holds_alternative<ks::ipc::msg::Ping>(ping->body)) {
        return false;
    }
    if (!send_message(answerer, Message{.request_id = request_id, .body = ks::ipc::msg::Pong{}})) {
        return false;
    }
    const std::optional<Message> pong = receive_message(asker);
    return pong && pong->request_id == request_id &&
           std::holds_alternative<ks::ipc::msg::Pong>(pong->body);
}

/// The longest path a Unix socket address holds, without its final NUL.
constexpr std::size_t kMaxSocketPath = sizeof(sockaddr_un::sun_path) - 1;

} // namespace

TEST_CASE("Supervisor socket: clients connect", "[supervisor-socket]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());
    const std::filesystem::path path = supervisor_socket_path(dir.path());
    REQUIRE(path == dir.path() / "supervisor.sock");

    {
        std::expected<Listener, std::error_code> listener = Listener::open(dir.path());
        REQUIRE(listener.has_value());
        REQUIRE(S_ISSOCK(mode_of(path)));
        REQUIRE((mode_of(path) & 07777U) == 0600U);

        const std::expected<std::optional<Fd>, std::error_code> none =
            listener->accept(::geteuid());
        REQUIRE(none.has_value());
        REQUIRE_FALSE(none->has_value());

        std::expected<Fd, std::error_code> first = connect_to_supervisor(dir.path(), ::geteuid());
        REQUIRE(first.has_value());
        std::expected<Fd, std::error_code> second = connect_to_supervisor(dir.path(), ::geteuid());
        REQUIRE(second.has_value());
        std::expected<std::optional<Fd>, std::error_code> first_accepted =
            accept_within(*listener, ::geteuid());
        REQUIRE(first_accepted.has_value());
        REQUIRE(first_accepted->has_value());
        std::expected<std::optional<Fd>, std::error_code> second_accepted =
            accept_within(*listener, ::geteuid());
        REQUIRE(second_accepted.has_value());
        REQUIRE(second_accepted->has_value());

        Connection client_one{std::move(*first)};
        Connection client_two{std::move(*second)};
        Connection server_one{std::move(*first_accepted).value_or(Fd{})};
        Connection server_two{std::move(*second_accepted).value_or(Fd{})};

        REQUIRE(ping_pong(client_one, server_one, 7));
        REQUIRE(ping_pong(server_one, client_one, 8));
        REQUIRE(ping_pong(client_two, server_two, 9));
        REQUIRE(ping_pong(server_two, client_two, 0xFFFF'FFFF));
    }
    REQUIRE_FALSE(std::filesystem::exists(std::filesystem::symlink_status(path)));
}

TEST_CASE("Supervisor socket: user IDs are checked", "[supervisor-socket]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());
    std::expected<Listener, std::error_code> listener = Listener::open(dir.path());
    REQUIRE(listener.has_value());
    const uid_t someone_else = ::geteuid() + 1;

    // Both ends of an accepted connection see this process's user ID.
    const std::expected<Fd, std::error_code> client_end =
        connect_to_supervisor(dir.path(), ::geteuid());
    REQUIRE(client_end.has_value());
    std::expected<std::optional<Fd>, std::error_code> server_end =
        accept_within(*listener, ::geteuid());
    REQUIRE(server_end.has_value());
    REQUIRE(server_end->has_value());
    const Fd server_socket = std::move(*server_end).value_or(Fd{});
    REQUIRE(peer_user_id(client_end->get()) == ::geteuid());
    REQUIRE(peer_user_id(server_socket.get()) == ::geteuid());

    // The listener refuses a client that isn't who it expects, and the client
    // finds its connection closed.
    std::expected<Fd, std::error_code> refused_client =
        connect_to_supervisor(dir.path(), ::geteuid());
    REQUIRE(refused_client.has_value());
    const std::expected<std::optional<Fd>, std::error_code> refused =
        accept_within(*listener, someone_else);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error() == std::errc::permission_denied);
    Connection client{std::move(*refused_client)};
    const std::expected<std::optional<ReceivedFrame>, ConnectionFailure> closed = client.receive();
    REQUIRE_FALSE(closed.has_value());
    REQUIRE(closed.error().error == ConnectionError::Closed);

    // A client refuses a listener that isn't who it expects. (Its connection
    // is left in the listener's backlog, so this check comes last.)
    const std::expected<Fd, std::error_code> wrong_listener =
        connect_to_supervisor(dir.path(), someone_else);
    REQUIRE_FALSE(wrong_listener.has_value());
    REQUIRE(wrong_listener.error() == std::errc::permission_denied);
}

TEST_CASE("Supervisor socket: what's at the path", "[supervisor-socket]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());
    const std::filesystem::path path = supervisor_socket_path(dir.path());

    SECTION("a socket left behind is replaced") {
        REQUIRE(leave_socket_file(path));
        REQUIRE(S_ISSOCK(mode_of(path)));
        const std::expected<Listener, std::error_code> listener = Listener::open(dir.path());
        REQUIRE(listener.has_value());
        REQUIRE(connect_to_supervisor(dir.path(), ::geteuid()).has_value());
    }

    SECTION("a regular file is refused and left alone") {
        {
            std::ofstream file{path};
            file << "not a socket";
        }
        REQUIRE_FALSE(Listener::open(dir.path()).has_value());
        REQUIRE(std::filesystem::is_regular_file(std::filesystem::symlink_status(path)));
        std::ifstream file{path};
        std::string contents;
        std::getline(file, contents);
        REQUIRE(contents == "not a socket");
    }

    SECTION("a symbolic link to a socket is refused and left alone") {
        const std::filesystem::path target = dir.path() / "elsewhere.sock";
        REQUIRE(leave_socket_file(target));
        std::filesystem::create_symlink(target, path);
        REQUIRE_FALSE(Listener::open(dir.path()).has_value());
        REQUIRE(std::filesystem::is_symlink(path));
        REQUIRE(std::filesystem::read_symlink(path) == target);
        REQUIRE(S_ISSOCK(mode_of(target)));
    }

    SECTION("the path must fit a socket address") {
        // A runtime directory whose socket path is exactly the longest that
        // fits, and one a byte longer.
        const std::size_t base = dir.path().native().size() + 1;
        const std::size_t name = ks::ipc::kSupervisorSocketName.size() + 1;
        REQUIRE(base + name < kMaxSocketPath);
        const std::filesystem::path fits =
            dir.path() / std::string(kMaxSocketPath - base - name, 'd');
        const std::filesystem::path too_long =
            dir.path() / std::string(kMaxSocketPath - base - name + 1, 'e');
        REQUIRE(supervisor_socket_path(fits).native().size() == kMaxSocketPath);
        REQUIRE(supervisor_socket_path(too_long).native().size() == kMaxSocketPath + 1);
        REQUIRE(::mkdir(fits.c_str(), 0700) == 0);
        REQUIRE(::mkdir(too_long.c_str(), 0700) == 0);

        const std::expected<Listener, std::error_code> fitting = Listener::open(fits);
        REQUIRE(fitting.has_value());
        REQUIRE(connect_to_supervisor(fits, ::geteuid()).has_value());

        const std::expected<Listener, std::error_code> refused = Listener::open(too_long);
        REQUIRE_FALSE(refused.has_value());
        REQUIRE(refused.error() == std::errc::filename_too_long);
        const std::expected<Fd, std::error_code> no_client =
            connect_to_supervisor(too_long, ::geteuid());
        REQUIRE_FALSE(no_client.has_value());
        REQUIRE(no_client.error() == std::errc::filename_too_long);
    }
}
