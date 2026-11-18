// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/track-loading/spec.md: kool_spin loading files
// onto decks end to end, with ks-engine on the null backend, and ks-worker or
// ks_fake_worker (requirements 9 to 21).

#include <ks/decode/decoder.h>
#include <ks/ipc/connection.h>
#include <ks/ipc/messages.h>
#include <ks/supervisor/spawn.h>

#include "decode_support.h"
#include "tracks_support.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using ks::ipc::ErrorCode;
using ks::logs::test::read_lines;
using ks::logs::test::wait_for_line;
using ks::logs::test::well_formed;
using ks::processes::test::exists;
using ks::track_loading::test::deck_shows;
using ks::track_loading::test::DeckView;
using ks::track_loading::test::error_of;
using ks::track_loading::test::eventually;
using ks::track_loading::test::FakeTrack;
using ks::track_loading::test::file_loaded;
using ks::track_loading::test::join;
using ks::track_loading::test::keeps_beating;
using ks::track_loading::test::kFakeWorkerPath;
using ks::track_loading::test::load;
using ks::track_loading::test::Milliseconds;
using ks::track_loading::test::open_ring;
using ks::track_loading::test::position_advances;
using ks::track_loading::test::Received;
using ks::track_loading::test::send_load;
using ks::track_loading::test::Session;
using ks::track_loading::test::set_playing;
using ks::track_loading::test::TrackClient;
using ks::track_loading::test::worker_pids;
using ks::track_loading::test::write_fake;
namespace msg = ks::ipc::msg;
using std::chrono::milliseconds;

/// The 1-second FLAC and MP3 that the decode-worker tests use.
std::filesystem::path flac() {
    return ks::decode::test::fixture("tone.flac");
}
std::filesystem::path mp3() {
    return ks::decode::test::fixture("tone.mp3");
}

/// True if, within `timeout`, deck `deck` shows track `id` with `frames`
/// frames ready, complete.
bool shows_complete(const TrackClient& client, std::size_t deck, std::uint64_t id,
                    std::uint64_t frames, Milliseconds timeout = Milliseconds{5'000}) {
    DeckView last;
    const bool shown = deck_shows(
        *client.decks, deck,
        [&](const DeckView& view) {
            return view.track_id == id && view.frames_ready == frames && view.complete;
        },
        last, timeout);
    CAPTURE(deck, id, frames, last.track_id, last.frames_ready, last.complete);
    return shown;
}

/// The text of the Error `answer` is, or nothing.
std::string error_text(const std::optional<Received>& answer) {
    const std::optional<msg::Error> error = error_of(answer);
    return error ? error->text : std::string{};
}

/// The FileLoaded `answer` is, which it must be.
msg::FileLoaded required_loaded(const std::optional<Received>& answer) {
    const std::optional<msg::FileLoaded> loaded = file_loaded(answer);
    CAPTURE(error_text(answer));
    REQUIRE(loaded.has_value());
    return loaded.value_or(msg::FileLoaded{});
}

/// The Error `answer` is, which it must be.
msg::Error required_error(const std::optional<Received>& answer) {
    const std::optional<msg::Error> error = error_of(answer);
    REQUIRE(error.has_value());
    return error.value_or(msg::Error{});
}

/// The request ID of `received`; 0 if nothing was received.
std::uint32_t request_id_of(const std::optional<Received>& received) {
    return received ? received->message.request_id : 0;
}

/// The track ID of the FileLoaded `answer` is, which it must be.
std::uint64_t loaded_id(const std::optional<Received>& answer) {
    const std::uint64_t id = required_loaded(answer).track_id;
    REQUIRE(id != 0);
    return id;
}

/// The code of the Error `answer` is, or nothing.
std::optional<ErrorCode> code_of(const std::optional<Received>& answer) {
    const std::optional<msg::Error> error = error_of(answer);
    return error ? std::optional<ErrorCode>{error->code} : std::nullopt;
}

/// Asks `session`'s kool_spin to stop through `client`, with request ID
/// `id`, and returns the messages that arrive until Done does, or until 5 s
/// pass.
std::vector<Received> stop(TrackClient& client, std::uint32_t id) {
    REQUIRE(ks::processes::test::send(client.connection,
                                      ks::ipc::Message{.request_id = id, .body = msg::Stop{}}));
    return ks::track_loading::test::receive_until(
        client.connection, [id](const std::vector<Received>& so_far) {
            return std::ranges::any_of(so_far, [id](const Received& r) {
                return r.message.request_id == id &&
                       std::holds_alternative<msg::Done>(r.message.body);
            });
        });
}

} // namespace

TEST_CASE("Supervisor tracks: loading a file", "[supervisor][tracks]") {
    Session session;
    TrackClient client = join(session, "loader");

    const msg::FileLoaded loaded = required_loaded(load(client, 1, flac()));
    CHECK(loaded.track_id != 0);
    CHECK(loaded.sample_rate == 44'100);
    CHECK(loaded.estimated_frames == 44'100);
    const std::uint64_t id = loaded.track_id;

    CHECK(shows_complete(client, 1, id, 44'100));
    ks::client_rings::test::ClientRing ring = open_ring(client);
    REQUIRE(ring.push(set_playing(1, true, 1)));
    CHECK(position_advances(*client.decks, 1));

    const std::string track = "track " + std::to_string(id);
    CHECK(
        wait_for_line(session.kool_spin_log(), "info", "started ks-worker (process ").has_value());
    CHECK(wait_for_line(session.kool_spin_log(), "info", ") for " + track).has_value());
    CHECK(wait_for_line(session.kool_spin_log(), "info", track + " loaded on deck 2").has_value());
    CHECK(
        wait_for_line(session.engine_log(), "info", "loaded " + track + " on deck 2").has_value());
    CHECK(eventually([&] {
        const std::vector<std::string> lines = read_lines(session.worker_log());
        return std::ranges::any_of(lines, [&](const std::string& line) {
            return line.find(track + ": ") != std::string::npos &&
                   line.find("decoded ") != std::string::npos;
        });
    }));
    CHECK(well_formed(read_lines(session.worker_log()), "ks-worker"));
}

TEST_CASE("Supervisor tracks: a load outlives its client", "[supervisor][tracks]") {
    const Session session;
    {
        TrackClient leaving = join(session, "leaving");
        static_cast<void>(send_load(leaving, 0, flac()));
    }
    const TrackClient watching = join(session, "watching");
    DeckView last;
    const bool shown = deck_shows(
        *watching.decks, 0,
        [](const DeckView& view) {
            return view.track_id != 0 && view.frames_ready == 44'100 && view.complete;
        },
        last);
    CAPTURE(last.track_id, last.frames_ready, last.complete);
    CHECK(shown);
}

TEST_CASE("Supervisor tracks: replacing a track releases it", "[supervisor][tracks]") {
    const Session session;
    TrackClient client = join(session, "replacer");

    const std::uint64_t first = loaded_id(load(client, 0, flac()));
    REQUIRE(shows_complete(client, 0, first, 44'100));
    const std::uint64_t second = loaded_id(load(client, 0, mp3()));
    CHECK(second != first);

    DeckView last;
    const bool shown = deck_shows(
        *client.decks, 0, [&](const DeckView& view) { return view.track_id == second; }, last);
    CHECK(shown);
    const std::string track = "track " + std::to_string(first);
    CHECK(wait_for_line(session.kool_spin_log(), "info", track + " released").has_value());
    CHECK(wait_for_line(session.engine_log(), "info", "released " + track).has_value());
}

TEST_CASE("Supervisor tracks: files it refuses", "[supervisor][tracks]") {
    Session session;
    TrackClient client = join(session, "refused");
    const std::filesystem::path dir = session.dir() / "files";
    REQUIRE(std::filesystem::create_directory(dir));

    // A path that doesn't exist.
    CHECK(code_of(load(client, 0, dir / "missing.flac")) == ErrorCode::Refused);

    // A directory.
    CHECK(code_of(load(client, 0, dir)) == ErrorCode::Refused);

    // A FIFO, which mustn't block the supervisor.
    const std::filesystem::path fifo = dir / "fifo";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    CHECK(code_of(load(client, 0, fifo)) == ErrorCode::Refused);
    const auto before = std::chrono::steady_clock::now();
    CHECK(ks::processes::test::pings(client.connection, client.next_request++));
    CHECK(std::chrono::steady_clock::now() - before < milliseconds{1'000});

    // A text file: the worker's refusal, word for word.
    const std::filesystem::path text = dir / "notes.txt";
    ks::logs::test::write_file(text, "These are not the samples you are looking for.\n");
    const std::expected<ks::decode::Decoder, ks::decode::Refusal> decoder =
        ks::decode::Decoder::open(ks::decode::test::source_for(text));
    REQUIRE_FALSE(decoder.has_value());
    const msg::Error refusal = required_error(load(client, 0, text));
    CHECK(refusal.code == ErrorCode::Refused);
    CHECK(refusal.text == decoder.error().text);

    // Each refusal is in the log.
    CHECK(eventually([&] {
        const std::vector<std::string> lines = read_lines(session.kool_spin_log());
        return std::ranges::count_if(lines, [](const std::string& line) {
                   return line.find(" warning ") != std::string::npos &&
                          line.find("refused client 'refused' a load") != std::string::npos;
               }) >= 4;
    }));

    // A relative path doesn't decode: BadMessage, and the connection closes.
    // encode() refuses it too, so the test writes the frame itself.
    std::vector<std::byte> body;
    const std::string_view relative = "music/a.flac";
    for (const std::uint64_t value : {std::uint64_t{0}, std::uint64_t{relative.size()}}) {
        for (std::size_t i = 0; i < 4; ++i) {
            body.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
        }
    }
    for (const char c : relative) {
        body.push_back(static_cast<std::byte>(c));
    }
    const std::uint32_t id = client.next_request++;
    REQUIRE(client.connection.send(static_cast<std::uint16_t>(ks::ipc::MessageType::LoadFile), id,
                                   body, {}));
    REQUIRE(client.connection.flush());
    const std::optional<Received> answer = ks::processes::test::receive(client.connection);
    CHECK(ks::processes::test::is<msg::Error>(answer, id));
    CHECK(code_of(answer) == ErrorCode::BadMessage);
    CHECK(ks::processes::test::closes(client.connection));
}

TEST_CASE("Supervisor tracks: the memory budget", "[supervisor][tracks]") {
    const Session session{kFakeWorkerPath, {"--track-memory", "1"}};
    TrackClient client = join(session, "budget");
    const std::filesystem::path& dir = session.dir();

    // 800,000 of 1,048,576 bytes.
    const std::uint64_t first =
        loaded_id(load(client, 0, write_fake(dir, "a", {.estimate = 100'000, .frames = 100'000})));
    REQUIRE(shows_complete(client, 0, first, 100'000));

    // Another 800,000 doesn't fit.
    const msg::Error too_big = required_error(
        load(client, 1, write_fake(dir, "b", {.estimate = 100'000, .frames = 100'000})));
    CHECK(too_big.code == ErrorCode::Refused);
    CHECK(too_big.text.find("MiB") != std::string::npos);

    // No estimate counts as 2 GiB.
    CHECK(code_of(load(client, 1, write_fake(dir, "c", {.estimate = 0, .frames = 1'000}))) ==
          ErrorCode::Refused);

    // Replacing the first frees its 800,000 once it's released.
    const std::uint64_t replacing =
        loaded_id(load(client, 0, write_fake(dir, "d", {.estimate = 20'000, .frames = 20'000})));
    REQUIRE(shows_complete(client, 0, replacing, 20'000));
    REQUIRE(wait_for_line(session.kool_spin_log(), "info",
                          "track " + std::to_string(first) + " released")
                .has_value());
    static_cast<void>(
        loaded_id(load(client, 1, write_fake(dir, "e", {.estimate = 100'000, .frames = 100'000}))));

    // 960,000 bytes in use. A track estimated at 80,000 bytes that decodes
    // only 8,000 fits, and once it has decoded, counts 8,000.
    const std::uint64_t short_one =
        loaded_id(load(client, 2, write_fake(dir, "f", {.estimate = 10'000, .frames = 1'000})));
    REQUIRE(shows_complete(client, 2, short_one, 1'000));
    static_cast<void>(
        loaded_id(load(client, 3, write_fake(dir, "g", {.estimate = 10'000, .frames = 10'000}))));
}

TEST_CASE("Supervisor tracks: four workers at most", "[supervisor][tracks]") {
    Session session{kFakeWorkerPath};
    TrackClient client = join(session, "busy");
    const std::filesystem::path dir = session.dir();

    std::vector<std::uint32_t> ids;
    for (std::uint32_t deck = 0; deck < 4; ++deck) {
        const FakeTrack hanging{.probe_hang = true, .ignore_sigterm = deck == 2};
        ids.push_back(
            send_load(client, deck, write_fake(dir, "hang" + std::to_string(deck), hanging)));
    }
    // No answer to any of them.
    std::this_thread::sleep_for(Milliseconds{500});
    std::expected<std::optional<ks::ipc::ReceivedFrame>, ks::ipc::ConnectionFailure> early =
        client.connection.receive();
    REQUIRE(early.has_value());
    CHECK_FALSE(early->has_value());

    // A fifth is Unavailable at once.
    const std::optional<Received> fifth =
        load(client, 0, write_fake(dir, "fifth", {.estimate = 1'000, .frames = 1'000}));
    CHECK(code_of(fifth) == ErrorCode::Unavailable);

    REQUIRE(eventually([&] { return worker_pids(session.kool_spin_log()).size() == 4; }));
    const auto pids = worker_pids(session.kool_spin_log());

    // Stopping answers the four, then Done, and leaves no worker behind,
    // even the one that blocks SIGTERM.
    const std::uint32_t stop_id = client.next_request++;
    const auto stopping_since = std::chrono::steady_clock::now();
    const std::vector<Received> received = stop(client, stop_id);
    std::set<std::uint32_t> answered;
    bool done = false;
    for (const Received& r : received) {
        if (std::holds_alternative<msg::Done>(r.message.body) && r.message.request_id == stop_id) {
            done = true;
        } else if (const auto* const error = std::get_if<msg::Error>(&r.message.body)) {
            CHECK_FALSE(done);
            CHECK(error->code == ErrorCode::NotSupported);
            answered.insert(r.message.request_id);
        }
    }
    CHECK(done);
    CHECK(answered == std::set<std::uint32_t>(ids.begin(), ids.end()));
    CHECK(session.process().wait(Milliseconds{3'000}) == 0);
    CHECK(std::chrono::steady_clock::now() - stopping_since < milliseconds{3'000});
    for (const auto& [track, pid] : pids) {
        CAPTURE(track, pid);
        CHECK(eventually([pid] { return !exists(pid); }, Milliseconds{1'000}));
    }
}

TEST_CASE("Supervisor tracks: the latest load wins", "[supervisor][tracks]") {
    const Session session{kFakeWorkerPath};
    TrackClient client = join(session, "racer");
    const std::filesystem::path& dir = session.dir();

    const std::uint32_t slow = send_load(
        client, 0,
        write_fake(dir, "slow", {.estimate = 1'000, .probe_delay_ms = 500, .frames = 1'000}));
    const std::uint32_t fast =
        send_load(client, 0, write_fake(dir, "fast", {.estimate = 2'000, .frames = 2'000}));

    const std::optional<Received> first = ks::processes::test::receive(client.connection);
    CHECK(request_id_of(first) == fast);
    const std::uint64_t id = loaded_id(first);
    const std::optional<Received> second = ks::processes::test::receive(client.connection);
    CHECK(request_id_of(second) == slow);
    CHECK(code_of(second) == ErrorCode::Refused);

    CHECK(shows_complete(client, 0, id, 2'000));
    std::this_thread::sleep_for(Milliseconds{1'000});
    CHECK(shows_complete(client, 0, id, 2'000));
}

TEST_CASE("Supervisor tracks: a worker dying while it decodes", "[supervisor][tracks]") {
    const Session session{kFakeWorkerPath};
    TrackClient client = join(session, "survivor");
    ks::client_rings::test::ClientRing ring = open_ring(client);

    const std::filesystem::path crashing = write_fake(session.dir(), "crash",
                                                      {.estimate = 20'480,
                                                       .frames = 20'480,
                                                       .chunk_frames = 4'096,
                                                       .chunk_ms = 50,
                                                       .end = "crash"});
    const std::uint64_t id = loaded_id(load(client, 0, crashing));
    REQUIRE(ring.push(set_playing(0, true, 1)));

    DeckView last;
    const bool ends = deck_shows(
        *client.decks, 0,
        [&](const DeckView& view) {
            return view.track_id == id && view.frames_ready == 20'480 && view.complete &&
                   view.position == 20'480.0 && !view.playing;
        },
        last);
    CAPTURE(last.track_id, last.frames_ready, last.complete, last.position, last.playing);
    CHECK(ends);
    CHECK(keeps_beating(*client.decks));
    CHECK(wait_for_line(session.kool_spin_log(), "error",
                        "track " + std::to_string(id) + ": ks-worker was killed by signal 9")
              .has_value());
}

TEST_CASE("Supervisor tracks: deleting the file while it plays", "[supervisor][tracks]") {
    const Session session;
    TrackClient client = join(session, "deleter");
    ks::client_rings::test::ClientRing ring = open_ring(client);
    const std::filesystem::path wav = session.dir() / "long.wav";
    ks::decode::test::write_long_wav(wav);

    const std::uint64_t id = loaded_id(load(client, 0, wav));
    REQUIRE(std::filesystem::remove(wav));
    REQUIRE(ring.push(set_playing(0, true, 1)));

    CHECK(shows_complete(client, 0, id, ks::decode::test::kLongFrames, Milliseconds{30'000}));
    CHECK(position_advances(*client.decks, 0));
}

TEST_CASE("Supervisor tracks: usage", "[supervisor][tracks]") {
    const ks::ipc::test::TempDir dir;
    REQUIRE(!dir.path().empty());
    const std::filesystem::path err = dir.path() / "stderr";

    /// Runs kool_spin with `options`, and returns its exit status.
    const auto run = [&](std::vector<std::string> options) {
        static_cast<void>(std::filesystem::remove(err));
        std::vector<std::string> args{
            "--runtime-dir", (dir.path() / "run").string(),
            "--log-dir",     (dir.path() / "logs").string(),
            "--engine",      std::string{ks::processes::test::kEnginePath}};
        args.insert(args.end(), options.begin(), options.end());
        args.insert(args.end(), {"--", "--backend", "null"});
        ks::processes::test::Process process = ks::processes::test::start(
            "/bin/sh", ks::logs::test::with_stderr(err, ks::processes::test::kKoolSpinPath, args));
        REQUIRE(process.pid() > 0);
        return process.wait(Milliseconds{10'000});
    };
    /// True if standard error has a line starting "kool_spin: ".
    const auto said = [&] {
        return std::ranges::any_of(read_lines(err), [](const std::string& line) {
            return line.starts_with("kool_spin: ");
        });
    };

    for (const std::vector<std::string>& bad : std::vector<std::vector<std::string>>{
             {"--track-memory", "0"},
             {"--track-memory", "1048577"},
             {"--track-memory", "x"},
             {"--track-memory", "1", "--track-memory", "2"},
         }) {
        CAPTURE(bad);
        CHECK(run(bad) == 2);
        CHECK(said());
    }

    CHECK(run({"--worker", (dir.path() / "no-such-worker").string()}) == 1);
    CHECK(said());
}
