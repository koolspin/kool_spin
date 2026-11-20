// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/client-rings/spec.md: the engine attaching and
// detaching rings (requirements 5 to 8). The test plays the supervisor, as
// in the processes tests: it creates the segments, and starts ks-engine as
// kool_spin would.

#include <ks/ipc/command.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/layout.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/region.h>
#include <ks/ipc/state.h>

#include "rings_support.h"
#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using ks::client_rings::test::attach;
using ks::client_rings::test::attached;
using ks::client_rings::test::ClientRing;
using ks::client_rings::test::create_segment;
using ks::client_rings::test::crossfader_applied;
using ks::client_rings::test::detach;
using ks::client_rings::test::is_error;
using ks::client_rings::test::kApplyWait;
using ks::client_rings::test::keeps_beating;
using ks::client_rings::test::real_code;
using ks::client_rings::test::set_crossfader;
using ks::client_rings::test::set_main_level;
using ks::client_rings::test::slot_of;
using ks::client_rings::test::start_engine;
using ks::client_rings::test::Supervised;
using ks::ipc::ErrorCode;
using ks::ipc::Fd;
using ks::ipc::Message;
using ks::processes::test::eventually;
using ks::processes::test::is;
using ks::processes::test::Milliseconds;
using ks::processes::test::pings;
using ks::processes::test::receive;
using ks::processes::test::Received;
using ks::processes::test::send;
namespace msg = ks::ipc::msg;

/// A ring segment and the test's own mapping of it, as a client would hold
/// it.
struct TestRing {
    Fd segment;
    ClientRing ring;
};

/// Creates a ring segment in `supervised`'s runtime directory, and maps it.
TestRing new_ring(Supervised& supervised) {
    Fd segment = create_segment(supervised);
    std::expected<ClientRing, std::string> ring = ClientRing::map(segment.get());
    REQUIRE(ring.has_value());
    return TestRing{.segment = std::move(segment), .ring = std::move(*ring)};
}

} // namespace

TEST_CASE("Engine rings: attach, apply, and detach", "[engine]") {
    const std::unique_ptr<Supervised> engine = start_engine();
    TestRing client = new_ring(*engine);

    const std::optional<Received> answer = attach(*engine, client.segment, 11);
    REQUIRE(is<msg::Attached>(answer, 11));
    CHECK(slot_of(answer) == 0U);

    REQUIRE(client.ring.push(set_crossfader(0.25, 1)));
    REQUIRE(client.ring.push(set_main_level(-6.0, 2)));
    CHECK(
        ks::client_rings::test::applied(*engine->state, 0, 2, [](const ks::ipc::MixerState& mixer) {
            return mixer.crossfader == real_code(0.25) && mixer.main_level_db == real_code(-6.0);
        }));
    const ks::ipc::ClientAck ack = engine->state->ack(0);
    CHECK(ack.commands_read == 2);
    CHECK(ack.rejected == 0);

    CHECK(is<msg::Detached>(detach(*engine, 0, 12), 12));
    CHECK(eventually([&] { return !attached(*engine->state, 0); }, kApplyWait));

    // The engine no longer reads the ring.
    REQUIRE(client.ring.push(set_crossfader(0.75, 3)));
    std::this_thread::sleep_for(Milliseconds{500});
    CHECK(engine->state->crossfader() == 0.25);
}

TEST_CASE("Engine rings: eight at most", "[engine]") {
    const std::unique_ptr<Supervised> engine = start_engine();
    std::vector<Fd> segments;
    for (std::uint32_t i = 0; i < 8; ++i) {
        CAPTURE(i);
        segments.push_back(create_segment(*engine));
        const std::optional<Received> answer = attach(*engine, segments.back(), 100 + i, i);
        REQUIRE(is<msg::Attached>(answer, 100 + i));
        CHECK(slot_of(answer) == i);
    }

    const Fd ninth = create_segment(*engine);
    CHECK(is_error(attach(*engine, ninth, 108, 0), ErrorCode::Unavailable, 108));

    REQUIRE(is<msg::Detached>(detach(*engine, 3, 200), 200));
    const Fd again = create_segment(*engine);
    const std::optional<Received> answer = attach(*engine, again, 201, 3);
    REQUIRE(is<msg::Attached>(answer, 201));
    CHECK(slot_of(answer) == 3U);
}

TEST_CASE("Engine rings: what it refuses", "[engine]") {
    const std::unique_ptr<Supervised> engine = start_engine();

    SECTION("segments that aren't rings") {
        const Fd state =
            create_segment(*engine, ks::ipc::SegmentKind::EngineState,
                           ks::ipc::kEngineStateLayoutVersion, ks::ipc::kEngineStateSegmentSize);
        CHECK(is_error(attach(*engine, state, 1), ErrorCode::Unavailable, 1));

        const Fd version_2 = create_segment(*engine, ks::ipc::SegmentKind::CommandRing, 2,
                                            ks::ipc::kCommandRingSegmentSize);
        CHECK(is_error(attach(*engine, version_2, 2), ErrorCode::Unavailable, 2));

        // One page: create() rounds the header's size up to a page, which is
        // smaller than a ring on every machine we build for.
        const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        REQUIRE(page < ks::ipc::kCommandRingSegmentSize);
        const Fd small =
            create_segment(*engine, ks::ipc::SegmentKind::CommandRing,
                           ks::ipc::kCommandRingLayoutVersion, ks::ipc::kSegmentHeaderSize);
        CHECK(is_error(attach(*engine, small, 3), ErrorCode::Unavailable, 3));
    }

    SECTION("detaching a slot that holds no ring") {
        CHECK(is_error(detach(*engine, 5, 4), ErrorCode::Unavailable, 4));
    }

    SECTION("detaching a slot twice") {
        const Fd segment = create_segment(*engine);
        const std::optional<Received> answer = attach(*engine, segment, 5);
        REQUIRE(is<msg::Attached>(answer, 5));
        const std::uint32_t slot = slot_of(answer).value_or(99);
        REQUIRE(send(*engine->socket, Message{.request_id = 6, .body = msg::Detach{.slot = slot}}));
        REQUIRE(send(*engine->socket, Message{.request_id = 7, .body = msg::Detach{.slot = slot}}));
        // The two answers, in either order: the first Detach completes, and
        // the second finds the slot already being detached, or free.
        const std::optional<Received> first = receive(*engine->socket);
        const std::optional<Received> second = receive(*engine->socket);
        const bool in_order =
            is<msg::Detached>(first, 6) && is_error(second, ErrorCode::Unavailable, 7);
        const bool swapped =
            is_error(first, ErrorCode::Unavailable, 7) && is<msg::Detached>(second, 6);
        CHECK((in_order || swapped));
    }

    CHECK(keeps_beating(*engine->state));
    CHECK(pings(*engine->socket, 50));
    const Fd valid = create_segment(*engine);
    CHECK(is<msg::Attached>(attach(*engine, valid, 51), 51));
}

TEST_CASE("Engine rings: a corrupt ring affects only its client", "[engine]") {
    const std::unique_ptr<Supervised> engine = start_engine();
    const TestRing first = new_ring(*engine);
    TestRing second = new_ring(*engine);
    const std::optional<Received> first_answer = attach(*engine, first.segment, 1);
    REQUIRE(is<msg::Attached>(first_answer, 1));
    const std::optional<Received> second_answer = attach(*engine, second.segment, 2, 1);
    REQUIRE(is<msg::Attached>(second_answer, 2));
    const std::uint32_t first_slot = slot_of(first_answer).value_or(99);
    const std::uint32_t second_slot = slot_of(second_answer).value_or(99);

    // The first ring's head, 1,000 past its tail: more than the ring holds.
    ks::ipc::CommandRing& shared = first.ring.ring();
    const std::uint64_t tail = ks::ipc::load_shared(shared.tail, std::memory_order_acquire);
    ks::ipc::store_shared(shared.head, tail + 1'000, std::memory_order_release);

    CHECK(eventually(
        [&] { return (engine->state->ack(first_slot).flags & ks::ipc::kClientCorrupt) != 0; },
        kApplyWait));
    REQUIRE(second.ring.push(set_crossfader(0.25, 1)));
    CHECK(crossfader_applied(*engine->state, second_slot, 0.25, 1));
}

TEST_CASE("Engine rings: work without the supervisor", "[engine]") {
    const std::unique_ptr<Supervised> engine = start_engine();
    TestRing client = new_ring(*engine);
    const std::optional<Received> answer = attach(*engine, client.segment, 1);
    REQUIRE(is<msg::Attached>(answer, 1));
    const std::uint32_t slot = slot_of(answer).value_or(99);

    engine->socket.reset();
    REQUIRE(client.ring.push(set_crossfader(0.25, 1)));
    CHECK(crossfader_applied(*engine->state, slot, 0.25, 1));

    engine->engine.signal(SIGTERM);
    CHECK(engine->engine.wait() == 0);
}
