// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/ks-play/spec.md: the client library, ks::client
// (requirements 12 to 20), against kool_spin on the null backend.

#include <ks/client/session.h>

#include <ks/client/command.h>
#include <ks/client/snapshot.h>
#include <ks/decode/decoder.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>

#include "decode_support.h"
#include "unload_support.h"
#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <thread>

namespace {

namespace client = ks::client;
using ks::track_loading::test::eventually;
using ks::track_loading::test::kFakeWorkerPath;
using ks::track_loading::test::Milliseconds;
using ks::track_loading::test::Session;
using ks::track_loading::test::write_fake;

std::filesystem::path flac() {
    return ks::decode::test::fixture("tone.flac");
}

/// A connected session, which must succeed.
client::Session connect(const Session& session, const std::string& name = "tester") {
    client::Result<client::Session> connected = client::Session::connect(name, session.dir());
    REQUIRE(connected.has_value());
    return std::move(*connected);
}

/// The snapshot of `s`, which must succeed.
client::Snapshot snapshot_of(const client::Session& s) {
    const client::Result<client::Snapshot> snapshot = s.snapshot();
    REQUIRE(snapshot.has_value());
    return snapshot.value_or(client::Snapshot{});
}

/// Sends `command` and waits until the engine has applied it.
client::Applied send_and_wait(client::Session& s, const client::Command& command) {
    const client::Result<std::uint64_t> sequence = s.send(command);
    REQUIRE(sequence.has_value());
    const client::Result<client::Applied> applied = s.wait_applied(sequence.value_or(0));
    REQUIRE(applied.has_value());
    return applied.value_or(client::Applied{});
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

/// Writes `value` into `lock`, as the engine would.
template <typename T>
void publish(ks::ipc::Seqlock<T>& lock, const T& value) {
    ks::ipc::SeqlockWriter<T> writer{lock};
    writer.write(value);
}

/// An engine state that holds valid values in every block.
std::unique_ptr<ks::ipc::EngineState> valid_state() {
    auto state = std::make_unique<ks::ipc::EngineState>();
    for (auto& deck : state->decks) {
        ks::ipc::DeckState value{};
        value.heartbeat = 1;
        publish(deck, value);
    }
    ks::ipc::MixerState mixer{};
    mixer.heartbeat = 1;
    for (auto& channel : mixer.channels) {
        channel.side = 2;
    }
    publish(state->mixer, mixer);
    ks::ipc::EngineStatus status{};
    status.heartbeat = 1;
    status.output_state = 1;
    publish(state->status, status);
    return state;
}

} // namespace

TEST_CASE("Session: connecting", "[client]") {
    const Session session;

    SECTION("a session, with a heartbeat that grows") {
        const client::Session connected = connect(session);
        const std::uint64_t first = snapshot_of(connected).engine.heartbeat;
        CHECK(eventually([&] { return snapshot_of(connected).engine.heartbeat > first; }));
    }

    SECTION("nothing to connect to") {
        const std::filesystem::path nowhere = session.dir() / "nowhere";
        const client::Result<client::Session> result = client::Session::connect("tester", nowhere);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == client::ErrorKind::NotReachable);
    }

    SECTION("a listener owned by someone else") {
        const client::Result<client::Session> result =
            client::Session::connect_as("tester", session.dir(), ::geteuid() + 1);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == client::ErrorKind::NotReachable);
    }

    SECTION("a name that's too long is refused before connecting") {
        // The runtime directory doesn't exist: a connection attempt would
        // say NotReachable.
        const client::Result<client::Session> result =
            client::Session::connect(std::string(65, 'n'), session.dir() / "nowhere");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == client::ErrorKind::InvalidArgument);
        const client::Result<client::Session> empty =
            client::Session::connect("", session.dir() / "nowhere");
        REQUIRE_FALSE(empty.has_value());
        CHECK(empty.error().kind == client::ErrorKind::InvalidArgument);
    }
}

TEST_CASE("Session: a ring and a command", "[client]") {
    const Session session;
    client::Session s = connect(session);

    // No ring yet: nothing is written.
    const client::Result<std::uint64_t> early = s.send(client::set_playing(0, true));
    REQUIRE_FALSE(early.has_value());
    CHECK(early.error().kind == client::ErrorKind::NoRing);

    REQUIRE(s.load_file(0, flac().string()).has_value());
    const client::Result<std::uint32_t> slot = s.open_ring();
    REQUIRE(slot.has_value());
    CHECK(slot.value_or(99) == 0);
    // The status block already shows the slot attached.
    CHECK(snapshot_of(s).clients[0].attached);
    const client::Result<std::uint32_t> again = s.open_ring();
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().kind == client::ErrorKind::RingAlreadyOpen);

    // Sequence numbers start at 1 and count up.
    const client::Result<std::uint64_t> first = s.send(client::set_playing(0, true));
    const client::Result<std::uint64_t> second = s.send(client::set_fader(0, 0.5));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first.value_or(0) == 1);
    CHECK(second.value_or(0) == 2);
    const client::Result<client::Applied> applied = s.wait_applied(2);
    REQUIRE(applied.has_value());
    CHECK(applied.value_or(client::Applied{}).outcome == client::Outcome::Applied);

    // The deck plays.
    CHECK(eventually([&] {
        const double before = snapshot_of(s).decks[0].position;
        std::this_thread::sleep_for(Milliseconds{200});
        return snapshot_of(s).decks[0].position > before;
    }));
}

TEST_CASE("Session: rejected commands", "[client]") {
    const Session session;
    {
        client::Session s = connect(session);
        REQUIRE(s.open_ring().has_value());

        const client::Applied nan = send_and_wait(s, client::set_rate(0, kNaN));
        CHECK(nan.outcome == client::Outcome::Rejected);
        CHECK(client::rejection_name(nan.rejection) == "bad value");

        const client::Applied side = send_and_wait(s, client::set_crossfader_assign(0, 9));
        CHECK(side.outcome == client::Outcome::Rejected);
        CHECK(client::rejection_name(side.rejection) == "bad value");

        const client::Applied target = send_and_wait(s, client::set_playing(7, true));
        CHECK(target.outcome == client::Outcome::Rejected);
        CHECK(client::rejection_name(target.rejection) == "bad target");

        // Commands after them are still applied: each call reports what the
        // engine did since the one before.
        const client::Applied after = send_and_wait(s, client::set_fader(0, 0.25));
        CHECK(after.outcome == client::Outcome::Applied);
        CHECK(eventually([&] { return snapshot_of(s).channels[0].fader == 0.25; }));
    }

    // A new session's ring doesn't see the earlier session's rejections.
    client::Session fresh = connect(session, "fresh");
    const client::Result<std::uint32_t> slot = fresh.open_ring();
    REQUIRE(slot.has_value());
    const client::Applied ok = send_and_wait(fresh, client::set_fader(1, 0.5));
    CHECK(ok.outcome == client::Outcome::Applied);
}

TEST_CASE("Session: the snapshot", "[client]") {
    SECTION("from kool_spin") {
        const Session session;
        client::Session s = connect(session);
        const client::Result<client::LoadedFile> loaded = s.load_file(1, flac().string());
        REQUIRE(loaded.has_value());
        const client::Result<std::uint32_t> slot = s.open_ring();
        REQUIRE(slot.has_value());

        CHECK(send_and_wait(s, client::set_rate(1, 1.02)).outcome == client::Outcome::Applied);
        CHECK(send_and_wait(s, client::set_fader(1, 0.5)).outcome == client::Outcome::Applied);
        CHECK(send_and_wait(s, client::set_crossfader_assign(1, 1)).outcome ==
              client::Outcome::Applied);
        CHECK(send_and_wait(s, client::set_main_level(-6.0)).outcome == client::Outcome::Applied);
        CHECK(send_and_wait(s, client::set_bus_map(2, 0)).outcome == client::Outcome::Applied);

        // The engine publishes after its next block.
        CHECK(eventually([&] {
            const client::Snapshot snap = snapshot_of(s);
            return snap.decks[1].rate == 1.02 && snap.channels[1].fader == 0.5 &&
                   snap.channels[1].side == client::Side::B && snap.mixer.main_level_db == -6.0 &&
                   snap.mixer.bus_main == 2 && snap.mixer.bus_cue == 0;
        }));
        const client::Snapshot snap = snapshot_of(s);
        const client::LoadedFile file = loaded.value_or(client::LoadedFile{});
        CHECK(snap.decks[1].track_id == file.track_id);
        CHECK(snap.decks[1].track_sample_rate == 44'100);
        CHECK(snap.decks[1].loaded);
        CHECK(eventually([&] { return snapshot_of(s).decks[1].complete; }));
        const client::ClientInfo& ack = snap.clients.at(slot.value_or(0));
        CHECK(ack.attached);
        CHECK(ack.commands_read == 5);
        CHECK(ack.last_sequence == 5);
        CHECK(ack.rejected == 0);
        CHECK(snap.engine.state == client::OutputState::Playing);
    }

    SECTION("a segment the test fills") {
        // Reals are data: NaN and infinities are kept.
        {
            const std::unique_ptr<ks::ipc::EngineState> state = valid_state();
            ks::ipc::DeckState deck{};
            deck.heartbeat = 2;
            deck.position = std::bit_cast<std::uint64_t>(kNaN);
            publish(state->decks[0], deck);
            ks::ipc::MixerState mixer{};
            mixer.heartbeat = 2;
            mixer.main_level_db = std::bit_cast<std::uint64_t>(-kInf);
            for (auto& channel : mixer.channels) {
                channel.side = 1;
            }
            publish(state->mixer, mixer);
            const client::Result<client::Snapshot> snap = client::Session::snapshot_from(*state);
            REQUIRE(snap.has_value());
            CHECK(std::isnan(snap.value_or(client::Snapshot{}).decks[0].position));
            CHECK(snap.value_or(client::Snapshot{}).mixer.main_level_db == -kInf);
        }

        // An invalid code makes the snapshot an error that names the field.
        {
            const std::unique_ptr<ks::ipc::EngineState> state = valid_state();
            ks::ipc::EngineStatus status{};
            status.heartbeat = 2;
            status.output_state = 9;
            publish(state->status, status);
            const client::Result<client::Snapshot> snap = client::Session::snapshot_from(*state);
            REQUIRE_FALSE(snap.has_value());
            CHECK(snap.error().kind == client::ErrorKind::BadState);
            CHECK(snap.error().text.find("output_state") != std::string::npos);
        }
        {
            const std::unique_ptr<ks::ipc::EngineState> state = valid_state();
            ks::ipc::MixerState mixer{};
            mixer.heartbeat = 2;
            mixer.channels[2].side = 7;
            publish(state->mixer, mixer);
            const client::Result<client::Snapshot> snap = client::Session::snapshot_from(*state);
            REQUIRE_FALSE(snap.has_value());
            CHECK(snap.error().text.find("side") != std::string::npos);
        }
        {
            const std::unique_ptr<ks::ipc::EngineState> state = valid_state();
            ks::ipc::EngineStatus status{};
            status.heartbeat = 2;
            status.output_state = 1;
            status.clients[3].last_rejection = 9;
            publish(state->status, status);
            const client::Result<client::Snapshot> snap = client::Session::snapshot_from(*state);
            REQUIRE_FALSE(snap.has_value());
            CHECK(snap.error().text.find("last_rejection") != std::string::npos);
        }

        // A seqlock left odd, as if the writer died mid-write.
        {
            const std::unique_ptr<ks::ipc::EngineState> state = valid_state();
            state->mixer.sequence |= 1U;
            const auto start = std::chrono::steady_clock::now();
            const client::Result<client::Snapshot> snap = client::Session::snapshot_from(*state);
            const auto took = std::chrono::steady_clock::now() - start;
            REQUIRE_FALSE(snap.has_value());
            CHECK(snap.error().kind == client::ErrorKind::BadState);
            CHECK(snap.error().text.find("mixer") != std::string::npos);
            CHECK(took < std::chrono::milliseconds{100});
        }
    }
}

TEST_CASE("Session: requests and notifications", "[client]") {
    Session session{kFakeWorkerPath};
    client::Session s = connect(session);

    SECTION("a load, its DecodeEnded, and requests around it") {
        const std::filesystem::path fake = write_fake(
            session.dir(), "track",
            {.estimate = 8'192, .frames = 8'192, .chunk_frames = 4'096, .chunk_ms = 100});
        const client::Result<client::LoadedFile> loaded = s.load_file(0, fake.string());
        REQUIRE(loaded.has_value());
        CHECK(loaded.value_or(client::LoadedFile{}).track_id != 0);
        CHECK(loaded.value_or(client::LoadedFile{}).sample_rate == 44'100);

        // The DecodeEnded arrives while other requests are made; each gets
        // its own reply.
        CHECK(s.ping().has_value());
        CHECK(s.unload_deck(3).has_value());
        CHECK(s.ping().has_value());

        const client::Result<ks::ipc::msg::DecodeEnded> ended = s.next_decode_ended();
        REQUIRE(ended.has_value());
        CHECK(ended.value_or(ks::ipc::msg::DecodeEnded{}).track_id ==
              loaded.value_or(client::LoadedFile{}).track_id);
        CHECK(ended.value_or(ks::ipc::msg::DecodeEnded{}).frames == 8'192);
        CHECK(ended.value_or(ks::ipc::msg::DecodeEnded{}).status == ks::ipc::TrackStatus::Complete);

        const client::Result<ks::ipc::msg::DecodeEnded> none =
            s.next_decode_ended(std::chrono::milliseconds{300});
        REQUIRE_FALSE(none.has_value());
        CHECK(none.error().kind == client::ErrorKind::Timeout);
    }

    SECTION("a refusal from kool_spin") {
        const client::Result<client::LoadedFile> refused =
            s.load_file(0, (session.dir() / "no-such-file").string());
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().kind == client::ErrorKind::Refused);
        CHECK(refused.error().code == ks::ipc::ErrorCode::Refused);
    }

    SECTION("stopping") {
        REQUIRE(s.stop().has_value());
        CHECK(session.process().wait(Milliseconds{5'000}) == 0);
        const auto start = std::chrono::steady_clock::now();
        const client::Result<void> late = s.ping();
        REQUIRE_FALSE(late.has_value());
        CHECK(late.error().kind == client::ErrorKind::Closed);
        CHECK(s.ping().error().kind == client::ErrorKind::Closed);
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{2});
    }
}
