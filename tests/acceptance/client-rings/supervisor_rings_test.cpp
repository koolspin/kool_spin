// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/client-rings/spec.md: kool_spin giving clients
// rings, end to end (requirements 9 to 14). Each test runs kool_spin in its
// own runtime directory, with ks-engine on miniaudio's null backend.

#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/state.h>

#include "rings_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using ks::client_rings::test::attached;
using ks::client_rings::test::Client;
using ks::client_rings::test::crossfader_applied;
using ks::client_rings::test::is_error;
using ks::client_rings::test::join;
using ks::client_rings::test::kApplyWait;
using ks::client_rings::test::KoolSpin;
using ks::client_rings::test::open_ring;
using ks::client_rings::test::open_ring_retrying;
using ks::client_rings::test::OpenedRing;
using ks::client_rings::test::OpenFailure;
using ks::client_rings::test::set_crossfader;
using ks::ipc::Connection;
using ks::ipc::ErrorCode;
using ks::ipc::Message;
using ks::processes::test::closes;
using ks::processes::test::connect;
using ks::processes::test::eventually;
using ks::processes::test::pings;
using ks::processes::test::receive;
using ks::processes::test::send;
namespace msg = ks::ipc::msg;

/// Joins `kool_spin` as `name`, or fails the test.
Client must_join(const KoolSpin& kool_spin, const std::string& name) {
    std::expected<Client, std::string> client = join(kool_spin.dir(), name);
    REQUIRE(client.has_value());
    return std::move(*client);
}

/// Joins `kool_spin` as `name`, trying again every 20 ms for up to 2 s if
/// that fails: just after many clients have left, the supervisor may still
/// count connections that have closed, and close a new one at once.
Client must_join_soon(const KoolSpin& kool_spin, const std::string& name) {
    std::expected<Client, std::string> client = join(kool_spin.dir(), name);
    static_cast<void>(eventually(
        [&] {
            if (!client) {
                client = join(kool_spin.dir(), name);
            }
            return client.has_value();
        },
        kApplyWait));
    REQUIRE(client.has_value());
    return std::move(*client);
}

/// Opens a ring for `client` with request ID 2, or fails the test.
OpenedRing must_open(Client& client) {
    std::expected<OpenedRing, OpenFailure> opened = open_ring(client.connection, 2);
    REQUIRE(opened.has_value());
    return std::move(*opened);
}

} // namespace

TEST_CASE("Supervisor rings: a client opens a ring", "[supervisor]") {
    const KoolSpin kool_spin;
    Client client = must_join(kool_spin, "ring");
    OpenedRing opened = must_open(client);
    CHECK(opened.slot == 0);

    REQUIRE(opened.ring.push(set_crossfader(0.25, 1)));
    CHECK(crossfader_applied(client.state, opened.slot, 0.25, 1));
}

TEST_CASE("Supervisor rings: what it refuses", "[supervisor]") {
    const KoolSpin kool_spin;

    SECTION("a second ring") {
        Client client = must_join(kool_spin, "greedy");
        OpenedRing opened = must_open(client);
        const std::expected<OpenedRing, OpenFailure> second = open_ring(client.connection, 3);
        REQUIRE(!second.has_value());
        CHECK(second.error() == OpenFailure{ErrorCode::NotSupported});
        REQUIRE(opened.ring.push(set_crossfader(0.3, 1)));
        CHECK(crossfader_applied(client.state, opened.slot, 0.3, 1));
    }

    SECTION("OpenRing before Hello") {
        std::expected<Connection, std::error_code> connection = connect(kool_spin.dir());
        REQUIRE(connection.has_value());
        REQUIRE(send(*connection, Message{.request_id = 1, .body = msg::OpenRing{}}));
        CHECK(is_error(receive(*connection), ErrorCode::BadMessage, 1));
        CHECK(closes(*connection));
    }

    SECTION("the engine's messages") {
        Client client = must_join(kool_spin, "impostor");
        // Any descriptor will do for the Attach: a file of the test's own.
        const ks::ipc::test::TempDir dir;
        REQUIRE(!dir.path().empty());
        const std::vector<ks::ipc::Fd> files = ks::ipc::test::open_files(dir.path(), 1);
        REQUIRE(files.size() == 1);
        const std::array<int, 1> fds{files.front().get()};
        REQUIRE(send(client.connection, Message{.request_id = 4, .body = msg::Attach{}}, fds));
        CHECK(is_error(receive(client.connection), ErrorCode::NotSupported, 4));
        REQUIRE(
            send(client.connection, Message{.request_id = 5, .body = msg::Attached{.slot = 0}}));
        CHECK(is_error(receive(client.connection), ErrorCode::NotSupported, 5));
        REQUIRE(send(client.connection, Message{.request_id = 6, .body = msg::Detach{.slot = 0}}));
        CHECK(is_error(receive(client.connection), ErrorCode::NotSupported, 6));
        REQUIRE(send(client.connection, Message{.request_id = 7, .body = msg::Detached{}}));
        CHECK(is_error(receive(client.connection), ErrorCode::NotSupported, 7));
        CHECK(pings(client.connection, 8));
    }
}

TEST_CASE("Supervisor rings: detached when the client leaves", "[supervisor]") {
    const KoolSpin kool_spin;
    Client watcher = must_join(kool_spin, "watcher");
    {
        Client leaving = must_join(kool_spin, "leaving");
        const OpenedRing opened = must_open(leaving);
        REQUIRE(opened.slot == 0);
        REQUIRE(eventually([&] { return attached(watcher.state, 0); }, kApplyWait));
    }
    CHECK(eventually([&] { return !attached(watcher.state, 0); }, kApplyWait));

    std::uint32_t id = 10;
    const std::expected<OpenedRing, OpenFailure> next = open_ring_retrying(watcher.connection, id);
    REQUIRE(next.has_value());
    CHECK(next->slot == 0);
}

TEST_CASE("Supervisor rings: eight at most, and none leaked", "[supervisor]") {
    const KoolSpin kool_spin;
    {
        std::vector<Client> clients;
        std::vector<OpenedRing> rings;
        for (std::uint32_t i = 0; i < 8; ++i) {
            CAPTURE(i);
            clients.push_back(must_join(kool_spin, std::format("client {}", i)));
            rings.push_back(must_open(clients.back()));
            CHECK(rings.back().slot == i);
        }

        Client ninth = must_join(kool_spin, "ninth");
        const std::expected<OpenedRing, OpenFailure> refused = open_ring(ninth.connection, 2);
        REQUIRE(!refused.has_value());
        CHECK(refused.error() == OpenFailure{ErrorCode::Unavailable});
        CHECK(pings(ninth.connection, 3));

        // One of the eight leaves, and the ninth gets its slot.
        clients.erase(clients.begin() + 4);
        std::uint32_t id = 10;
        const std::expected<OpenedRing, OpenFailure> fourth =
            open_ring_retrying(ninth.connection, id);
        REQUIRE(fourth.has_value());
        CHECK(fourth->slot == 4);
    }

    // Everyone has left. Twenty clients ask for a ring, and leave without
    // waiting for the answer.
    for (int i = 0; i < 20; ++i) {
        CAPTURE(i);
        std::expected<Connection, std::error_code> connection = connect(kool_spin.dir());
        REQUIRE(connection.has_value());
        REQUIRE(send(*connection,
                     Message{.request_id = 1, .body = msg::Hello{std::format("hasty {}", i)}}));
        REQUIRE(send(*connection, Message{.request_id = 2, .body = msg::OpenRing{}}));
    }

    // No slot was leaked: eight new clients each get one.
    std::vector<Client> clients;
    std::vector<OpenedRing> rings;
    std::set<std::uint32_t> slots;
    for (int i = 0; i < 8; ++i) {
        CAPTURE(i);
        clients.push_back(must_join_soon(kool_spin, std::format("again {}", i)));
        std::uint32_t next_id = 2;
        std::expected<OpenedRing, OpenFailure> opened =
            open_ring_retrying(clients.back().connection, next_id);
        REQUIRE(opened.has_value());
        slots.insert(opened->slot);
        rings.push_back(std::move(*opened));
    }
    CHECK(slots == std::set<std::uint32_t>{0, 1, 2, 3, 4, 5, 6, 7});
}

TEST_CASE("Supervisor rings: rings outlive the supervisor", "[supervisor]") {
    KoolSpin kool_spin;
    Client client = must_join(kool_spin, "survivor");
    OpenedRing opened = must_open(client);
    const pid_t engine = kool_spin.engine();
    REQUIRE(engine > 0);

    kool_spin.process().signal(SIGKILL);
    CHECK(kool_spin.process().wait().has_value());
    CHECK(closes(client.connection));

    REQUIRE(opened.ring.push(set_crossfader(0.25, 1)));
    CHECK(crossfader_applied(client.state, opened.slot, 0.25, 1));

    REQUIRE(::kill(engine, SIGTERM) == 0);
    CHECK(eventually([&] { return kool_spin.engine() == 0; }));
}
