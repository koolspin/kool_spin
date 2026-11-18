// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/ks-play/spec.md: kool_spin unloading decks and
// telling clients how decoding ended, end to end, with ks-engine on the null
// backend, and ks-worker or ks_fake_worker (requirements 5 to 10).

#include <ks/decode/decoder.h>
#include <ks/ipc/connection.h>
#include <ks/ipc/frame.h>
#include <ks/ipc/messages.h>

#include "decode_support.h"
#include "unload_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

using ks::ipc::ErrorCode;
using ks::ipc::TrackStatus;
using ks::ks_play::test::first_of;
using ks::ks_play::test::has;
using ks::ks_play::test::has_any;
using ks::ks_play::test::read_for;
using ks::ks_play::test::send_unload;
using ks::logs::test::read_lines;
using ks::logs::test::wait_for_line;
using ks::track_loading::test::deck_shows;
using ks::track_loading::test::DeckView;
using ks::track_loading::test::eventually;
using ks::track_loading::test::join;
using ks::track_loading::test::kFakeWorkerPath;
using ks::track_loading::test::load;
using ks::track_loading::test::Milliseconds;
using ks::track_loading::test::receive_until;
using ks::track_loading::test::Received;
using ks::track_loading::test::send_load;
using ks::track_loading::test::Session;
using ks::track_loading::test::TrackClient;
using ks::track_loading::test::worker_pids;
using ks::track_loading::test::write_fake;
namespace msg = ks::ipc::msg;

std::filesystem::path flac() {
    return ks::decode::test::fixture("tone.flac");
}

/// True if deck `deck` shows no track within 5 s.
bool shows_empty(const TrackClient& client, std::size_t deck) {
    DeckView last;
    const bool shown = deck_shows(
        *client.decks, deck, [](const DeckView& view) { return view.track_id == 0; }, last);
    CAPTURE(deck, last.track_id);
    return shown;
}

/// Sends an UnloadDeck for `deck` on `client`, and returns what arrives
/// until its answer does.
std::vector<Received> unload(TrackClient& client, std::uint32_t deck, std::uint32_t& id) {
    id = client.next_request++;
    REQUIRE(send_unload(client.connection, id, deck));
    return receive_until(client.connection, [id](const std::vector<Received>& so_far) {
        return has<msg::DeckUnloaded>(so_far, id) || std::ranges::any_of(so_far, [id](auto& r) {
                   return r.message.request_id == id &&
                          std::holds_alternative<msg::Error>(r.message.body);
               });
    });
}

} // namespace

TEST_CASE("Supervisor unload: a loaded deck", "[supervisor][unload]") {
    const Session session;
    TrackClient client = join(session, "unloader");

    const auto loaded = ks::track_loading::test::file_loaded(load(client, 2, flac()));
    REQUIRE(loaded.has_value());
    DeckView last;
    REQUIRE(deck_shows(
        *client.decks, 2, [&](const DeckView& view) { return view.track_id == loaded->track_id; },
        last));

    std::uint32_t id = 0;
    const std::vector<Received> answer = unload(client, 2, id);
    CHECK(has<msg::DeckUnloaded>(answer, id));
    CHECK(shows_empty(client, 2));

    const std::string track =
        "track " + std::to_string(loaded.value_or(msg::FileLoaded{}).track_id);
    CHECK(wait_for_line(session.kool_spin_log(), "info", track + " released").has_value());
    CHECK(wait_for_line(session.kool_spin_log(), "info", "client 'unloader' asked to unload deck 3")
              .has_value());
    CHECK(wait_for_line(session.engine_log(), "info", "unloaded deck 3").has_value());
}

TEST_CASE("Supervisor unload: an unload cancels a load", "[supervisor][unload]") {
    const Session session{kFakeWorkerPath};
    TrackClient client = join(session, "canceller");

    const std::uint32_t load_id =
        send_load(client, 0,
                  write_fake(session.dir(), "slow",
                             {.estimate = 1'000, .probe_delay_ms = 1'000, .frames = 1'000}));
    std::uint32_t unload_id = 0;
    std::vector<Received> received = unload(client, 0, unload_id);
    CHECK(has<msg::DeckUnloaded>(received, unload_id));

    // The load's answer comes once its probe does.
    std::vector<Received> later =
        receive_until(client.connection, [load_id](const std::vector<Received>& so_far) {
            return std::ranges::any_of(
                so_far, [load_id](const Received& r) { return r.message.request_id == load_id; });
        });
    received.insert(received.end(), std::make_move_iterator(later.begin()),
                    std::make_move_iterator(later.end()));
    const auto refused = std::ranges::find_if(
        received, [load_id](const Received& r) { return r.message.request_id == load_id; });
    REQUIRE(refused != received.end());
    const auto* const error = std::get_if<msg::Error>(&refused->message.body);
    REQUIRE(error != nullptr);
    CHECK(error->code == ErrorCode::Refused);
    CHECK(error->text.find("unloaded") != std::string::npos);

    std::this_thread::sleep_for(Milliseconds{2'000});
    const std::optional<DeckView> view = client.decks->deck(0);
    CHECK((!view || view->track_id == 0));
}

TEST_CASE("Supervisor unload: a track still decoding", "[supervisor][unload]") {
    const Session session{kFakeWorkerPath};
    TrackClient client = join(session, "decoder");

    // 10 minutes at 44,100 Hz, decoded at about real time.
    const std::uint32_t load_id = send_load(client, 0,
                                            write_fake(session.dir(), "long",
                                                       {.estimate = 26'460'000,
                                                        .frames = 26'460'000,
                                                        .chunk_frames = 4'096,
                                                        .chunk_ms = 100}));
    std::vector<Received> received =
        receive_until(client.connection, [load_id](const std::vector<Received>& so_far) {
            return has<msg::FileLoaded>(so_far, load_id);
        });
    REQUIRE(has<msg::FileLoaded>(received, load_id));

    const auto pids = worker_pids(session.kool_spin_log());
    REQUIRE(pids.size() == 1);
    const pid_t worker = pids.front().second;

    std::uint32_t id = 0;
    std::vector<Received> answer = unload(client, 0, id);
    CHECK(has<msg::DeckUnloaded>(answer, id));
    received.insert(received.end(), std::make_move_iterator(answer.begin()),
                    std::make_move_iterator(answer.end()));

    CHECK(
        eventually([&] { return ::kill(worker, 0) != 0 && errno == ESRCH; }, Milliseconds{2'000}));

    if (!has_any<msg::DecodeEnded>(received)) {
        std::vector<Received> more =
            receive_until(client.connection, [](const std::vector<Received>& so_far) {
                return has_any<msg::DecodeEnded>(so_far);
            });
        received.insert(received.end(), std::make_move_iterator(more.begin()),
                        std::make_move_iterator(more.end()));
    }
    const auto ended = first_of<msg::DecodeEnded>(received);
    REQUIRE(ended.has_value());
    const msg::DecodeEnded result = ended.value_or(msg::DecodeEnded{});
    CHECK(result.track_id == pids.front().first);
    CHECK((result.status == TrackStatus::Failed || result.status == TrackStatus::Truncated));
}

TEST_CASE("Supervisor unload: DecodeEnded", "[supervisor][unload]") {
    SECTION("only the client that asked, after FileLoaded") {
        const Session session;
        TrackClient asker = join(session, "asker");
        TrackClient bystander = join(session, "bystander");

        const std::uint32_t id = send_load(asker, 0, flac());
        const std::vector<Received> received =
            receive_until(asker.connection, [](const std::vector<Received>& so_far) {
                return has_any<msg::DecodeEnded>(so_far);
            });
        REQUIRE(received.size() == 2);
        // FileLoaded first, always, then the DecodeEnded.
        CHECK(std::holds_alternative<msg::FileLoaded>(received.front().message.body));
        CHECK(received.front().message.request_id == id);
        const auto* const ended = std::get_if<msg::DecodeEnded>(&received.back().message.body);
        REQUIRE(ended != nullptr);
        CHECK(received.back().message.request_id == 0);
        CHECK(ended->frames == 44'100);
        CHECK(ended->status == TrackStatus::Complete);
        const auto* const loaded = std::get_if<msg::FileLoaded>(&received.front().message.body);
        REQUIRE(loaded != nullptr);
        CHECK(ended->track_id == loaded->track_id);

        CHECK(read_for(bystander.connection, Milliseconds{1'000}).empty());
    }

    SECTION("a track whose worker dies") {
        const Session session{kFakeWorkerPath};
        TrackClient client = join(session, "survivor");
        const std::uint32_t id = send_load(client, 0,
                                           write_fake(session.dir(), "crash",
                                                      {.estimate = 20'480,
                                                       .frames = 20'480,
                                                       .chunk_frames = 4'096,
                                                       .chunk_ms = 50,
                                                       .end = "crash"}));
        const std::vector<Received> received =
            receive_until(client.connection, [](const std::vector<Received>& so_far) {
                return has_any<msg::DecodeEnded>(so_far);
            });
        REQUIRE(has<msg::FileLoaded>(received, id));
        const auto ended = first_of<msg::DecodeEnded>(received);
        REQUIRE(ended.has_value());
        const msg::DecodeEnded result = ended.value_or(msg::DecodeEnded{});
        CHECK(result.frames == 20'480);
        CHECK(result.status == TrackStatus::Failed);
    }

    SECTION("decoding that ends at once still follows FileLoaded") {
        const Session session{kFakeWorkerPath};
        TrackClient client = join(session, "quick");
        const std::uint32_t id = send_load(
            client, 0, write_fake(session.dir(), "quick", {.estimate = 1'000, .frames = 1'000}));
        const std::vector<Received> received =
            receive_until(client.connection, [](const std::vector<Received>& so_far) {
                return has_any<msg::DecodeEnded>(so_far);
            });
        REQUIRE(received.size() == 2);
        CHECK(std::holds_alternative<msg::FileLoaded>(received.front().message.body));
        CHECK(received.front().message.request_id == id);
        CHECK(std::holds_alternative<msg::DecodeEnded>(received.back().message.body));
    }

    SECTION("a client that leaves") {
        const Session session{kFakeWorkerPath};
        {
            TrackClient leaving = join(session, "leaver");
            const std::uint32_t id = send_load(
                leaving, 0,
                write_fake(
                    session.dir(), "slow",
                    {.estimate = 8'192, .frames = 8'192, .chunk_frames = 4'096, .chunk_ms = 300}));
            const std::vector<Received> received =
                receive_until(leaving.connection, [id](const std::vector<Received>& so_far) {
                    return has<msg::FileLoaded>(so_far, id);
                });
            REQUIRE(has<msg::FileLoaded>(received, id));
        }
        // No worker is left, and nothing was reported as an error.
        CHECK(eventually([&] {
            const auto pids = worker_pids(session.kool_spin_log());
            return !pids.empty() && ::kill(pids.front().second, 0) != 0;
        }));
        std::this_thread::sleep_for(Milliseconds{500});
        CHECK_FALSE(ks::logs::test::find_line(read_lines(session.kool_spin_log()), "error", "")
                        .has_value());
    }
}

TEST_CASE("Supervisor unload: what it refuses", "[supervisor][unload]") {
    const Session session;
    SECTION("deck 4") {
        TrackClient client = join(session, "bad-deck");
        std::vector<std::byte> body(4, std::byte{0});
        body.front() = std::byte{4};
        const ks::ipc::Frame frame{
            .type = 24, .request_id = 5, .descriptor_count = 0, .body = body};
        REQUIRE(client.connection.send(frame, {}));
        REQUIRE(client.connection.flush());
        CHECK(ks::processes::test::error_code(ks::processes::test::receive(client.connection)) ==
              ErrorCode::BadMessage);
        CHECK(ks::processes::test::closes(client.connection));
    }

    SECTION("messages that aren't for the supervisor") {
        TrackClient client = join(session, "wrong-way");
        REQUIRE(ks::processes::test::send(
            client.connection, ks::ipc::Message{.request_id = 7, .body = msg::DeckUnloaded{}}));
        const auto first = ks::processes::test::receive(client.connection);
        CHECK(ks::processes::test::error_code(first) == ErrorCode::NotSupported);
        REQUIRE(ks::processes::test::send(
            client.connection,
            ks::ipc::Message{.request_id = 0,
                             .body = msg::DecodeEnded{
                                 .track_id = 1, .frames = 1, .status = TrackStatus::Complete}}));
        const auto second = ks::processes::test::receive(client.connection);
        CHECK(ks::processes::test::error_code(second) == ErrorCode::NotSupported);
        // Still connected.
        REQUIRE(ks::processes::test::send(client.connection,
                                          ks::ipc::Message{.request_id = 9, .body = msg::Ping{}}));
        CHECK(
            ks::processes::test::is<msg::Pong>(ks::processes::test::receive(client.connection), 9));
    }
}
