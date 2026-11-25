// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/supervisor-crash/spec.md: what plays on, and who
// keeps control, when kool_spin dies (requirements 14 to 17).

#include <ks/client/command.h>
#include <ks/client/session.h>
#include <ks/client/snapshot.h>
#include <ks/ipc/command.h>
#include <ks/ipc/connection.h>
#include <ks/ipc/listener.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/segment.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/spsc_ring.h>
#include <ks/ipc/state.h>

#include "relaunch_support.h"
#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace ks::supervisor_crash::test;
namespace client = ks::client;
namespace restart = ks::engine_restart::test;

/// Sends `command` and waits for the engine: what it says.
client::Applied send_and_wait(client::Session& session, const client::Command& command) {
    const client::Result<std::uint64_t> sequence = session.send(command);
    REQUIRE(sequence.has_value());
    const client::Result<client::Applied> applied = session.wait_applied(sequence.value_or(0));
    REQUIRE(applied.has_value());
    return applied.value_or(client::Applied{});
}

client::Snapshot snapshot_of(const client::Session& session) {
    const client::Result<client::Snapshot> snapshot = session.snapshot();
    REQUIRE(snapshot.has_value());
    return snapshot.value_or(client::Snapshot{});
}

} // namespace

TEST_CASE("Supervisor crash: playback has no gap", "[crash]") {
    const Files files;
    auto session = playing(restart::write_sine(files.path(), "sine.wav", 120));
    // Start looking once the track has settled.
    std::this_thread::sleep_for(Milliseconds{500});
    const std::uint64_t first = session->recording.reader().frames_written();
    const pid_t engine = session->session.engine();
    REQUIRE(engine > 0);

    session->session.process().signal(SIGKILL);
    REQUIRE(session->session.process().wait() == 128 + SIGKILL);

    const std::uint64_t at_kill = session->recording.reader().frames_written();
    std::this_thread::sleep_for(Milliseconds{2'000});
    const std::uint64_t last = session->recording.reader().frames_written();
    CHECK(last >= at_kill + static_cast<std::uint64_t>(1.5 * kRate));
    CHECK(session->recording.reader().starts() == 1);
    CHECK(session->session.engine() == engine);
    const std::vector<float> samples =
        restart::recorded_from(session->recording, first, last - first);
    REQUIRE(samples.size() == last - first);
    const restart::GapCheck check = restart::check_no_gap(samples, 500);
    CAPTURE(check.crossings, check.widest, check.narrowest);
    CHECK(check.ok);
}

TEST_CASE("Supervisor crash: a track still decoding finishes", "[crash]") {
    const Files files;
    constexpr std::uint32_t kSeconds = 600;
    const std::filesystem::path wav =
        restart::write_constant(files.path(), "constant.wav", kSeconds);
    const auto complete = [](const std::optional<ks::ipc::DeckState>& deck) {
        return deck && (deck->flags & ks::ipc::kDeckComplete) != 0;
    };

    // FileLoaded comes as decoding starts, and 10 minutes take longer to
    // decode (0.2 s) than kool_spin takes to die, unless the machine is busy:
    // a run that finds the track decoded says nothing, so try again. (The
    // worker can't be frozen meanwhile: a stopped process whose parent dies
    // is sent SIGHUP.)
    bool said = false;
    for (int run = 0; run < 5 && !said; ++run) {
        auto session = restart::start_playing(wav);
        const auto workers = ks::track_loading::test::worker_pids(session->session.kool_spin_log());
        REQUIRE(workers.size() == 1);
        session->session.process().signal(SIGKILL);
        REQUIRE(session->session.process().wait() == 128 + SIGKILL);
        said = !complete(session->client.state.deck(0));
        CHECK(eventually(
            [&] {
                const auto deck = session->client.state.deck(0);
                return complete(deck) && deck->frames_ready == std::uint64_t{kSeconds} * kRate;
            },
            Milliseconds{30'000}));
        CHECK(eventually([&] { return ended(workers.front().second); }));
    }
    CHECK(said);
}

TEST_CASE("Supervisor crash: a session keeps control", "[crash]") {
    const Files files;
    const std::filesystem::path wav = restart::write_constant(files.path(), "constant.wav", 120);
    restart::Session kool_spin;
    client::Result<client::Session> connected =
        client::Session::connect("survivor", kool_spin.dir());
    REQUIRE(connected.has_value());
    client::Session session = std::move(*connected);
    REQUIRE(session.load_file(0, wav.string()).has_value());
    REQUIRE(session.next_decode_ended(Milliseconds{30'000}).has_value());
    REQUIRE(session.open_ring().has_value());
    REQUIRE(send_and_wait(session, client::set_playing(0, true)).outcome ==
            client::Outcome::Applied);

    kool_spin.process().signal(SIGKILL);
    REQUIRE(kool_spin.process().wait() == 128 + SIGKILL);

    const auto still_works = [&](double position, double crossfader) {
        CHECK(send_and_wait(session, client::set_playing(0, false)).outcome ==
              client::Outcome::Applied);
        CHECK(send_and_wait(session, client::seek(0, position)).outcome ==
              client::Outcome::Applied);
        CHECK(send_and_wait(session, client::set_crossfader(crossfader)).outcome ==
              client::Outcome::Applied);
        CHECK(eventually([&] {
            const client::Snapshot now = snapshot_of(session);
            return !now.decks[0].playing && now.decks[0].position == position &&
                   now.mixer.crossfader == crossfader;
        }));
        CHECK(send_and_wait(session, client::set_playing(0, true)).outcome ==
              client::Outcome::Applied);
        CHECK(eventually([&] {
            const client::Snapshot now = snapshot_of(session);
            return now.decks[0].playing && now.decks[0].position > position;
        }));
        // A rejection is still reported, with its reason.
        const client::Applied rejected =
            send_and_wait(session, client::set_rate(0, std::numeric_limits<double>::quiet_NaN()));
        CHECK(rejected.outcome == client::Outcome::Rejected);
        CHECK(rejected.rejection == client::Rejection::BadValue);
        const std::uint64_t beat = snapshot_of(session).engine.heartbeat;
        CHECK(eventually([&] { return snapshot_of(session).engine.heartbeat > beat; }));
    };

    // Nothing has touched the socket: the session hasn't noticed.
    CHECK(session.connected());
    still_works(1'000.0, 0.25);

    // Now it has.
    for (int i = 0; i < 2; ++i) {
        const client::Result<void> pong = session.ping();
        REQUIRE_FALSE(pong.has_value());
        CHECK(pong.error().kind == client::ErrorKind::Closed);
        CHECK_FALSE(session.connected());
    }
    still_works(2'000.0, 0.75);

    // Every other request is over, at once.
    const Clock::time_point began = Clock::now();
    const auto closed = [](const auto& result) {
        return !result && result.error().kind == client::ErrorKind::Closed;
    };
    CHECK(closed(session.load_file(0, wav.string())));
    CHECK(closed(session.unload_deck(0)));
    CHECK(closed(session.stop()));
    CHECK(closed(session.open_ring()));
    CHECK(closed(session.next_decode_ended()));
    CHECK(Clock::now() - began < Milliseconds{1'000});
    still_works(3'000.0, 0.5);

    session.close();
    CHECK_FALSE(session.connected());
    CHECK(closed(session.send(client::set_playing(0, false))));
    CHECK(closed(session.wait_applied(1)));
    CHECK(closed(session.snapshot()));
}

TEST_CASE("Supervisor crash: a queued DecodeEnded outlives the connection", "[crash]") {
    const Files files;
    const std::filesystem::path wav = restart::write_constant(files.path(), "constant.wav", 1);
    restart::Session kool_spin;
    client::Result<client::Session> session = client::Session::connect("survivor", kool_spin.dir());
    REQUIRE(session.has_value());
    const client::Result<client::LoadedFile> loaded = session->load_file(0, wav.string());
    REQUIRE(loaded.has_value());
    // The DecodeEnded comes while this waits, or is in the socket already,
    // ahead of the Pong.
    std::this_thread::sleep_for(Milliseconds{2'000});
    REQUIRE(session->ping().has_value());

    kool_spin.process().signal(SIGKILL);
    REQUIRE(kool_spin.process().wait() == 128 + SIGKILL);
    CHECK_FALSE(session->ping().has_value());
    REQUIRE_FALSE(session->connected());

    const client::Result<ks::ipc::msg::DecodeEnded> ended = session->next_decode_ended();
    REQUIRE(ended.has_value());
    CHECK(ended.value_or(ks::ipc::msg::DecodeEnded{}).track_id ==
          loaded.value_or(client::LoadedFile{}).track_id);
    const client::Result<ks::ipc::msg::DecodeEnded> none = session->next_decode_ended();
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().kind == client::ErrorKind::Closed);
}

namespace {

/// A supervisor and an engine, played by the test: what a Session talks to.
/// It answers one Hello and one OpenRing, with slot 3, and shows the slot
/// attached.
struct FakeKoolSpin {
    ks::ipc::test::TempDir dir;
    std::unique_ptr<ks::ipc::SegmentFactory> factory;
    ks::ipc::Fd state_fd;
    ks::ipc::Fd ring_fd;
    std::unique_ptr<ks::ipc::Region> state_region;
    std::unique_ptr<ks::ipc::Region> ring_region;
    ks::ipc::EngineState* state = nullptr;
    ks::ipc::CommandRing* ring = nullptr;
    std::unique_ptr<ks::ipc::Listener> listener;
    std::unique_ptr<ks::ipc::Connection> connection;
    bool served = false;

    static constexpr std::uint32_t kSlot = 3;

    FakeKoolSpin() {
        auto made = ks::ipc::SegmentFactory::open(dir.path());
        REQUIRE(made.has_value());
        factory = std::make_unique<ks::ipc::SegmentFactory>(std::move(*made));
        auto state_segment =
            factory->create(ks::ipc::SegmentKind::EngineState, ks::ipc::kEngineStateLayoutVersion,
                            ks::ipc::kEngineStateSegmentSize);
        auto ring_segment =
            factory->create(ks::ipc::SegmentKind::CommandRing, ks::ipc::kCommandRingLayoutVersion,
                            ks::ipc::kCommandRingSegmentSize);
        REQUIRE(state_segment.has_value());
        REQUIRE(ring_segment.has_value());
        state_fd = std::move(*state_segment);
        ring_fd = std::move(*ring_segment);
        auto mapped_state =
            ks::ipc::Region::map(state_fd.get(), ks::ipc::SegmentKind::EngineState,
                                 ks::ipc::kEngineStateLayoutVersion, ks::ipc::Access::ReadWrite);
        auto mapped_ring =
            ks::ipc::Region::map(ring_fd.get(), ks::ipc::SegmentKind::CommandRing,
                                 ks::ipc::kCommandRingLayoutVersion, ks::ipc::Access::ReadWrite);
        REQUIRE(mapped_state.has_value());
        REQUIRE(mapped_ring.has_value());
        state_region = std::make_unique<ks::ipc::Region>(std::move(*mapped_state));
        ring_region = std::make_unique<ks::ipc::Region>(std::move(*mapped_ring));
        state = state_region->at<ks::ipc::EngineState>(ks::ipc::kSegmentHeaderSize);
        ring = ring_region->at<ks::ipc::CommandRing>(ks::ipc::kSegmentHeaderSize);
        REQUIRE(state != nullptr);
        REQUIRE(ring != nullptr);
        auto opened = ks::ipc::Listener::open(dir.path());
        REQUIRE(opened.has_value());
        listener = std::make_unique<ks::ipc::Listener>(std::move(*opened));
        show(attached(0));
    }

    /// An attached slot that has read `commands` commands.
    static ks::ipc::ClientAck attached(std::uint64_t commands) {
        ks::ipc::ClientAck ack{};
        ack.flags = ks::ipc::kClientAttached;
        ack.commands_read = commands;
        ack.last_sequence = commands;
        return ack;
    }

    /// Publishes a status block whose slot kSlot is `ack`.
    void show(const ks::ipc::ClientAck& ack) const {
        ks::ipc::EngineStatus status{};
        status.heartbeat = 1;
        status.output_state = 1;
        std::span{status.clients}[kSlot] = ack;
        ks::ipc::SeqlockWriter<ks::ipc::EngineStatus> writer{state->status};
        writer.write(status);
    }

    /// Answers the Hello and the OpenRing, on a thread of its own.
    void serve() {
        std::optional<ks::ipc::Fd> accepted;
        static_cast<void>(eventually([&] {
            auto got = listener->accept(::geteuid());
            if (got && *got) {
                accepted = std::move(**got);
            }
            return accepted.has_value();
        }));
        if (!accepted) {
            return;
        }
        connection = std::make_unique<ks::ipc::Connection>(std::move(*accepted));
        for (int i = 0; i < 2; ++i) {
            const auto received = ks::processes::test::receive(*connection);
            if (!received) {
                return;
            }
            const std::uint32_t id = received->message.request_id;
            const bool hello = std::holds_alternative<ks::ipc::msg::Hello>(received->message.body);
            const std::array<int, 1> fds{hello ? state_fd.get() : ring_fd.get()};
            const ks::ipc::Message answer =
                hello ? ks::ipc::Message{.request_id = id, .body = ks::ipc::msg::Welcome{}}
                      : ks::ipc::Message{.request_id = id,
                                         .body = ks::ipc::msg::RingOpened{.slot = kSlot}};
            if (!ks::processes::test::send(*connection, answer, fds)) {
                return;
            }
        }
        served = true;
    }
};

} // namespace

TEST_CASE("Supervisor crash: a reused slot isn't mistaken for ours", "[crash]") {
    FakeKoolSpin fake;
    std::thread server{[&fake] { fake.serve(); }};
    client::Result<client::Session> session = client::Session::connect("dropped", fake.dir.path());
    const client::Result<std::uint32_t> slot =
        session ? session->open_ring() : std::unexpected{session.error()};
    server.join();
    REQUIRE(session.has_value());
    REQUIRE(slot == FakeKoolSpin::kSlot);
    REQUIRE(fake.served);

    // Two commands; the engine takes the first.
    REQUIRE(session->send(client::set_crossfader(0.1)) == 1U);
    REQUIRE(session->send(client::set_crossfader(0.2)) == 2U);
    ks::ipc::RingConsumer<ks::ipc::CommandSlot, ks::ipc::kCommandRingCapacity> engine{*fake.ring};
    REQUIRE(engine.try_pop().has_value());

    // The connection closes, and the slot is another client's: far ahead.
    fake.connection.reset();
    CHECK_FALSE(session->ping().has_value());
    REQUIRE_FALSE(session->connected());
    fake.show(FakeKoolSpin::attached(900));

    const auto outcome = [&](std::uint64_t sequence) {
        const client::Result<client::Applied> applied =
            session->wait_applied(sequence, Milliseconds{200});
        REQUIRE(applied.has_value());
        return applied.value_or(client::Applied{}).outcome;
    };
    CHECK(outcome(1) == client::Outcome::Applied);
    CHECK(outcome(2) == client::Outcome::TimedOut);
    // Not sent at all.
    CHECK(outcome(3) == client::Outcome::TimedOut);

    REQUIRE(engine.try_pop().has_value());
    CHECK(outcome(2) == client::Outcome::Applied);

    // A tail that makes no sense isn't progress.
    REQUIRE(session->send(client::set_crossfader(0.3)) == 3U);
    ks::ipc::store_shared(fake.ring->tail, std::uint64_t{1'000'000}, std::memory_order_release);
    CHECK(outcome(3) != client::Outcome::Applied);
}
