// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/engine-restart/spec.md: kool_spin restarting
// ks-engine (requirements 1 to 13), and the chaos tests that read the
// recording (requirement 14 onwards). Every test starts kool_spin, so none
// can run in the build loop's sandbox; the maintainer runs them outside it.

#include <ks/ipc/command.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/recording.h>
#include <ks/ipc/state.h>
#include <ks/supervisor/run.h>

#include "restart_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace ks::engine_restart::test;
using ks::ipc::CommandType;
using ks::ipc::ErrorCode;
using ks::track_loading::test::FakeTrack;
using ks::track_loading::test::kFakeWorkerPath;
namespace msg = ks::ipc::msg;

/// A temporary folder for the audio files, apart from the session's own.
using Files = ks::ipc::test::TempDir;

/// The deck block of deck `deck`, once it's playing: within 5 s.
std::optional<ks::ipc::DeckState> playing_deck(const Client& client, std::size_t deck,
                                               double past_frames = 0.0) {
    std::optional<ks::ipc::DeckState> last;
    static_cast<void>(eventually([&] {
        last = client.state.deck(deck);
        return last && loaded(*last) && playing(*last) && real_of(last->position) > past_frames;
    }));
    if (last && loaded(*last) && playing(*last) && real_of(last->position) > past_frames) {
        return last;
    }
    return std::nullopt;
}

/// True if, within `timeout`, the engine has restarted `restarts` times: the
/// recording has that many more starts, and has grown since the last.
bool restarted(const Playing& playing, std::uint64_t starts, Milliseconds timeout = kRestartWait) {
    return eventually(
        [&] {
            const auto& reader = playing.recording.reader();
            const auto boundaries = reader.boundaries();
            return reader.starts() >= starts && !boundaries.empty() &&
                   reader.frames_written() > boundaries.back().frame + (std::uint64_t{2} * kPeriod);
        },
        timeout);
}

/// True if, within 5 s, client slot `slot` is attached and has read up to
/// `sequence`.
bool applied(const State& state, std::size_t slot, std::uint64_t sequence) {
    return eventually([&] {
        const ks::ipc::ClientAck ack = state.ack(slot);
        return (ack.flags & ks::ipc::kClientAttached) != 0 && ack.last_sequence == sequence;
    });
}

/// A command for deck `deck` with a real argument.
ks::ipc::CommandSlot deck_command(CommandType type, std::uint32_t deck, double value,
                                  std::uint64_t sequence) {
    ks::ipc::CommandSlot command = ks::client_rings::test::command(type, value, sequence);
    command.target = deck;
    return command;
}

/// The mean of `samples`.
double mean(const std::vector<float>& samples) {
    if (samples.empty()) {
        return 0.0;
    }
    return std::accumulate(samples.begin(), samples.end(), 0.0) /
           static_cast<double>(samples.size());
}

/// The number of descriptors process `pid` has open, or nothing where /proc
/// isn't there.
std::optional<std::size_t> open_descriptors(pid_t pid) {
#ifdef __linux__
    std::error_code error;
    std::size_t count = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator{"/proc/" + std::to_string(pid) + "/fd", error}) {
        static_cast<void>(entry);
        ++count;
    }
    if (error) {
        return std::nullopt;
    }
    return count;
#else
    static_cast<void>(pid);
    return std::nullopt;
#endif
}

} // namespace

TEST_CASE("Restart: kill -9 resumes at the right position", "[restart]") {
    const Files files;
    const std::filesystem::path wav = write_constant(files.path(), "constant.wav", 600);
    auto playing_session = start_playing(wav);
    Playing& p = *playing_session;
    p.play(0);
    const auto first = require(playing_deck(p.client, 0, 0.5 * kRate));
    const std::uint64_t track_id = first.track_id;

    // What the deck was doing, and what had been recorded, just before.
    const auto before = require(p.client.state.deck(0));
    const std::uint64_t written_before = p.recording.reader().frames_written();
    const Clock::time_point read_at = Clock::now();
    REQUIRE(written_before > 4'000);
    const auto steady_before = require(p.recording.left(written_before - 2'000, 1'000));
    const double steady = mean(steady_before);
    REQUIRE(steady > 0.1);

    const auto [old_pid, killed_at] = kill_engine(p.session);
    REQUIRE(restarted(p, 2));
    const Milliseconds took = std::chrono::duration_cast<Milliseconds>(Clock::now() - killed_at);
    std::cout << "restart time: " << took.count() << " ms\n";
    CHECK(took <= kRestartLimit);
    CHECK(p.session.engine() != old_pid);
    CHECK(p.session.process().running());

    // The deck: the same track, playing, where it would be.
    const auto after = require(playing_deck(p.client, 0));
    CHECK(after.track_id == track_id);
    const double expected = position_at(before, after.host_time);
    const double error_frames = real_of(after.position) - expected;
    CAPTURE(expected, real_of(after.position));
    CHECK(std::abs(error_frames) < 0.010 * kRate);

    // The recording: one new boundary, where the old engine stopped.
    const auto boundaries = p.recording.reader().boundaries();
    REQUIRE(boundaries.size() == 2);
    const auto elapsed_frames =
        static_cast<std::uint64_t>(std::chrono::duration<double>(killed_at - read_at).count() *
                                   kRate) +
        1;
    CHECK(boundaries[1].frame >= written_before);
    // The paced engine catches up after a stall by rendering periods back to
    // back, so on a busy machine `written_before` can be several periods
    // behind the clock; allow 16.
    CHECK(boundaries[1].frame <= written_before + elapsed_frames + (std::uint64_t{16} * kPeriod));
    CHECK(boundaries[1].host_time > host_ns(killed_at));

    // The fade-in: the first frames are silent, the output rises, without
    // dropping, to what it was, over about 50 ms.
    REQUIRE(eventually(
        [&] { return p.recording.reader().frames_written() >= boundaries[1].frame + 6'000; }));
    const auto fade = require(p.recording.left(boundaries[1].frame, 6'000));
    CHECK(std::abs(static_cast<double>(fade[0])) < 0.02 * steady);
    std::size_t reached = fade.size();
    for (std::size_t i = 1; i < fade.size(); ++i) {
        CHECK(fade[i] >= fade[i - 1] - 1e-4F);
        if (reached == fade.size() && static_cast<double>(fade[i]) >= 0.99 * steady) {
            reached = i;
        }
    }
    CAPTURE(reached);
    CHECK(static_cast<double>(reached) >= 0.030 * kRate);
    CHECK(static_cast<double>(reached) <= 0.100 * kRate);
    for (std::size_t i = reached; i < fade.size(); ++i) {
        CHECK(std::abs(static_cast<double>(fade[i]) - steady) < 0.02 * steady);
    }
}

TEST_CASE("Restart: decks, mixer, cue point, and rings come back", "[restart]") {
    const Files files;
    const std::filesystem::path constant = write_constant(files.path(), "constant.wav", 600);
    const std::filesystem::path sine = write_sine(files.path(), "sine.wav", 600);
    auto playing_session = start_playing(constant);
    Playing& p = *playing_session;
    REQUIRE(is_loaded(load(p.client, 1, sine)));

    // A second client with a ring of its own.
    std::expected<Client, std::string> second = join(p.session.dir(), "second");
    REQUIRE(second.has_value());
    auto second_ring = open_ring(*second);
    REQUIRE(second_ring.slot == 1);

    // Deck 0 plays with a cue point; deck 1 is paused at frame 100000; the
    // crossfader is at 0.5.
    p.play(0);
    REQUIRE(p.ring.ring.push(deck_command(CommandType::SetCuePoint, 0, 33'000.0, p.sequence++)));
    REQUIRE(p.ring.ring.push(deck_command(CommandType::Seek, 1, 100'000.0, p.sequence++)));
    p.send(CommandType::SetCrossfader, 0, 0.5);
    REQUIRE(applied(p.client.state, 0, p.sequence - 1));
    REQUIRE(playing_deck(p.client, 0, 0.5 * kRate).has_value());
    const auto deck0 = require(p.client.state.deck(0));
    const auto deck1 = require(p.client.state.deck(1));
    REQUIRE_FALSE(playing(deck1));
    REQUIRE(real_of(deck1.position) == 100'000.0);
    const auto mixer = require(p.client.state.mixer());
    CHECK(real_of(mixer.crossfader) == 0.5);

    kill_engine(p.session);
    // A command written while the engine is away.
    REQUIRE(second_ring.ring.push(
        ks::client_rings::test::command(CommandType::SetCrossfader, 0.25, 1)));
    REQUIRE(restarted(p, 2));

    const auto after0 = require(playing_deck(p.client, 0));
    CHECK(after0.track_id == deck0.track_id);
    CHECK(real_of(after0.cue) == 33'000.0);
    const auto after1 = require(p.client.state.deck(1));
    CHECK(after1.track_id == deck1.track_id);
    CHECK_FALSE(playing(after1));
    CHECK(std::abs(real_of(after1.position) - 100'000.0) <= 1.0);

    // Each ring is where it was, and works: the command written during the
    // outage is applied, and so is one written now.
    CHECK(applied(p.client.state, 1, 1));
    const auto mixer_after = require(p.client.state.mixer());
    CHECK(real_of(mixer_after.crossfader) == 0.25);
    p.send(CommandType::SetCrossfader, 0, 0.75);
    CHECK(applied(p.client.state, 0, p.sequence - 1));
    CHECK(eventually([&] {
        const auto block = p.client.state.mixer();
        return block && real_of(block->crossfader) == 0.75;
    }));
}

TEST_CASE("Restart: killed while decoding", "[restart]") {
    // A track that takes about 10 s to decode, so that the engine dies in
    // the middle of it.
    const Files files;
    const std::filesystem::path track =
        ks::track_loading::test::write_fake(files.path(), "slow.fake",
                                            FakeTrack{.estimate = 44'100ULL * 30,
                                                      .frames = 44'100ULL * 30,
                                                      .chunk_frames = 4'096,
                                                      .chunk_ms = 40});
    SessionOptions options;
    options.worker = std::string{kFakeWorkerPath};
    auto playing_session = start_playing(track, options);
    Playing& p = *playing_session;
    p.play(0);
    REQUIRE(playing_deck(p.client, 0, 0.2 * kRate).has_value());
    const auto before = require(p.client.state.deck(0));
    REQUIRE(before.frames_ready < 44'100ULL * 30);

    kill_engine(p.session);
    REQUIRE(restarted(p, 2));

    // The decode carries on, and finishes; the client is told.
    bool ended = false;
    for (int i = 0; i < 20 && !ended; ++i) {
        const std::optional<Received> note = ks::processes::test::receive(p.client.connection);
        if (note && std::holds_alternative<msg::DecodeEnded>(note->message.body)) {
            const auto& body = std::get<msg::DecodeEnded>(note->message.body);
            ended = body.track_id == before.track_id && body.frames == 44'100ULL * 30;
        }
    }
    CHECK(ended);
    const auto after = require(playing_deck(p.client, 0));
    CHECK(after.track_id == before.track_id);
    CHECK(eventually([&] {
        const auto block = p.client.state.deck(0);
        return block && (block->flags & ks::ipc::kDeckComplete) != 0 &&
               block->frames_ready == 44'100ULL * 30;
    }));
}

TEST_CASE("Restart: killed with a load in flight", "[restart]") {
    const Files files;
    const std::filesystem::path constant = write_constant(files.path(), "constant.wav", 60);
    const std::filesystem::path track = ks::track_loading::test::write_fake(
        files.path(), "late.fake",
        FakeTrack{.estimate = 44'100ULL * 10, .probe_delay_ms = 600, .frames = 44'100ULL * 10});
    SessionOptions options;
    options.worker = std::string{kFakeWorkerPath};
    Session session{options};
    std::expected<Client, std::string> client = join(session.dir(), "late");
    REQUIRE(client.has_value());
    const auto supervisor_pid = session.process().pid();
    static_cast<void>(constant);
    // A first load, on deck 2, so that what the supervisor opens once, such
    // as ks-worker.log, is open before the count.
    const std::filesystem::path warm = ks::track_loading::test::write_fake(
        files.path(), "warm.fake", FakeTrack{.estimate = 44'100, .frames = 44'100});
    REQUIRE(is_loaded(load(*client, 1, warm)));
    // Its DecodeEnded.
    const std::optional<Received> decoded = ks::processes::test::receive(client->connection);
    REQUIRE(decoded.has_value());
    std::this_thread::sleep_for(Milliseconds{500});
    const std::optional<std::size_t> descriptors_before = open_descriptors(supervisor_pid);

    // The engine can't answer the LoadTrack: it's stopped, then killed.
    const std::uint32_t id = client->next_request++;
    REQUIRE(ks::processes::test::send(
        client->connection,
        ks::ipc::Message{.request_id = id, .body = msg::LoadFile{.deck = 0, .path = track}}));
    const pid_t engine = session.engine();
    REQUIRE(engine > 0);
    std::this_thread::sleep_for(Milliseconds{100});
    REQUIRE(::kill(engine, SIGSTOP) == 0);
    std::this_thread::sleep_for(Milliseconds{1'500});
    REQUIRE(::kill(engine, SIGKILL) == 0);

    const std::optional<Received> answer = ks::processes::test::receive(client->connection);
    REQUIRE(ks::processes::test::is<msg::Error>(answer, id));
    const auto error = require(error_of(answer));
    CHECK(error.code == ErrorCode::Unavailable);
    CHECK(error.text.starts_with("ks-engine restarted"));

    REQUIRE(session.engine_replaced(engine));
    // Nothing is left of the load: no deck holds a track, no worker runs,
    // and the supervisor holds the descriptors it did before.
    CHECK(eventually([&] {
        const auto deck = client->state.deck(0);
        return !deck || !loaded(*deck);
    }));
    for (const auto& [track_id, pid] :
         ks::track_loading::test::worker_pids(session.kool_spin_log())) {
        static_cast<void>(track_id);
        CHECK(eventually([&] { return !ks::processes::test::exists(pid); }));
    }
    if (descriptors_before) {
        CHECK(eventually([&] { return open_descriptors(supervisor_pid) == descriptors_before; }));
    }
}

TEST_CASE("Restart: requests during a restart", "[restart]") {
    const Files files;
    const std::filesystem::path constant = write_constant(files.path(), "constant.wav", 60);
    SessionOptions options;
    options.engine_options = {"--start-delay", "2000"};
    Session session{options};
    std::expected<Client, std::string> client = join(session.dir(), "waiting");
    REQUIRE(client.has_value());

    const pid_t old_pid = session.engine();
    REQUIRE(old_pid > 0);
    REQUIRE(::kill(old_pid, SIGKILL) == 0);
    // The new engine is held in start-up for 2 s.
    REQUIRE(eventually([&] {
        const pid_t now = session.engine();
        return now > 0 && now != old_pid;
    }));

    for (const auto& [what, request] :
         {std::pair<std::string, int>{"LoadFile", 0}, std::pair<std::string, int>{"OpenRing", 1},
          std::pair<std::string, int>{"UnloadDeck", 2}}) {
        CAPTURE(what);
        std::optional<Received> answer;
        if (request == 0) {
            answer = load(*client, 0, constant);
        } else if (request == 1) {
            answer = ask(*client, msg::OpenRing{});
        } else {
            answer = ask(*client, msg::UnloadDeck{.deck = 0});
        }
        const auto error = require(error_of(answer));
        CHECK(error.code == ErrorCode::Unavailable);
        CHECK(error.text == "ks-engine is restarting");
    }
    CHECK(ks::processes::test::pings(client->connection, client->next_request++));

    // A client that connects now is welcomed.
    const std::expected<Client, std::string> late = join(session.dir(), "late");
    CHECK(late.has_value());

    // Once the engine is up, the same requests work.
    CHECK(eventually(
        [&] {
            const auto answer = load(*client, 0, constant);
            return is_loaded(answer);
        },
        Milliseconds{10'000}));
}

TEST_CASE("Restart: stop during a restart", "[restart]") {
    SessionOptions options;
    options.engine_options = {"--start-delay", "3000"};
    Session session{options};
    std::expected<Client, std::string> client = join(session.dir(), "stopper");
    REQUIRE(client.has_value());
    const pid_t old_pid = session.engine();
    REQUIRE(old_pid > 0);
    REQUIRE(::kill(old_pid, SIGKILL) == 0);
    REQUIRE(session.engine_replaced(old_pid));
    const pid_t starting = session.engine();

    const auto answer = ask(*client, msg::Stop{});
    CHECK(ks::processes::test::is<msg::Done>(answer, client->next_request - 1));
    CHECK(session.process().wait(Milliseconds{3'000}) == 0);
    CHECK_FALSE(ks::processes::test::exists(starting));
    CHECK(session.engine() == 0);
}

TEST_CASE("Restart: the crash-loop guard", "[restart]") {
    SECTION("restarts twice, restarts clean the third time") {
        SessionOptions options;
        options.engine = std::string{ks::processes::test::kProbePath};
        options.engine_options = {"--mode", "crash"};
        options.wait_for_socket = false;
        Session session{options};
        // The probe crashes after Ready, twice; the third time it's given
        // --clean and stays up.
        CHECK(eventually(
            [&] {
                return ks::logs::test::find_line(
                           ks::logs::test::read_lines(session.kool_spin_log()), "info",
                           "restarting ks-engine (restart 3)")
                    .has_value();
            },
            Milliseconds{10'000}));
        std::this_thread::sleep_for(Milliseconds{1'000});
        CHECK(session.process().running());
        CHECK(session.engine() > 0);
        const auto lines = ks::logs::test::read_lines(session.kool_spin_log());
        CHECK(ks::logs::test::find_line(lines, "info", "restarting ks-engine (restart 1)"));
        CHECK(ks::logs::test::find_line(lines, "info", "restarting ks-engine (restart 2)"));
        CHECK_FALSE(ks::logs::test::find_line(lines, "info", "restarting ks-engine (restart 4)"));
    }

    SECTION("a fourth exit gives up") {
        SessionOptions options;
        options.engine = std::string{ks::processes::test::kProbePath};
        options.engine_options = {"--mode", "crash-always"};
        options.wait_for_socket = false;
        Session session{options};
        CHECK(session.process().wait(Milliseconds{10'000}) == 3);
        CHECK(session.stderr_text().find("ks-engine keeps failing; not restarting it") !=
              std::string::npos);
        CHECK(session.engine() == 0);
        CHECK_FALSE(std::filesystem::exists(session.dir() / "supervisor.sock"));
    }

    SECTION("a real engine killed three times comes back clean") {
        const Files files;
        const std::filesystem::path constant = write_constant(files.path(), "constant.wav", 600);
        auto playing_session = start_playing(constant);
        Playing& p = *playing_session;
        p.play(0);
        REQUIRE(playing_deck(p.client, 0, 0.2 * kRate).has_value());
        for (std::uint64_t kill = 1; kill <= 2; ++kill) {
            kill_engine(p.session);
            REQUIRE(restarted(p, kill + 1));
            // Resumed, as before: the deck is playing the same track.
            REQUIRE(playing_deck(p.client, 0).has_value());
        }
        kill_engine(p.session);
        REQUIRE(restarted(p, 4));
        // Clean: no track on any deck, nothing playing, and the ring's slot
        // is still attached.
        CHECK(eventually([&] {
            const auto deck = p.client.state.deck(0);
            return deck && !loaded(*deck) && !playing(*deck);
        }));
        p.send(CommandType::SetCrossfader, 0, 0.25);
        CHECK(applied(p.client.state, 0, p.sequence - 1));
    }

    SECTION("exits more than the window apart count as the first again") {
        const Files files;
        const std::filesystem::path constant = write_constant(files.path(), "constant.wav", 600);
        SessionOptions options;
        options.supervisor = {"--crash-window", "500"};
        auto playing_session = start_playing(constant, options);
        Playing& p = *playing_session;
        p.play(0);
        REQUIRE(playing_deck(p.client, 0, 0.2 * kRate).has_value());
        for (std::uint64_t kill = 1; kill <= 3; ++kill) {
            kill_engine(p.session);
            REQUIRE(restarted(p, kill + 1));
            // Every restart resumes: the deck still plays its track.
            REQUIRE(playing_deck(p.client, 0).has_value());
            std::this_thread::sleep_for(Milliseconds{700});
        }
    }
}

TEST_CASE("Restart: a start that fails counts", "[restart]") {
    // The probe crashes the first time, and then never sends Ready, unless it
    // is given --clean. With a 500 ms limit, each late start is killed and
    // counts, and the third exit makes the clean restart.
    SessionOptions options;
    options.engine = std::string{ks::processes::test::kProbePath};
    options.engine_options = {"--mode", "crash-then-silent"};
    options.supervisor = {"--ready-timeout", "500"};
    options.wait_for_socket = false;
    Session session{options};
    CHECK(eventually(
        [&] {
            return ks::logs::test::find_line(ks::logs::test::read_lines(session.kool_spin_log()),
                                             "info", "restarting ks-engine (restart 3)")
                .has_value();
        },
        Milliseconds{15'000}));
    CHECK(eventually([&] { return std::filesystem::exists(session.dir() / "supervisor.sock"); }));
    CHECK(session.process().running());
}

TEST_CASE("Restart: the log goes on", "[restart]") {
    const Files files;
    const std::filesystem::path constant = write_constant(files.path(), "constant.wav", 600);
    auto playing_session = start_playing(constant);
    Playing& p = *playing_session;
    p.play(0);
    REQUIRE(playing_deck(p.client, 0, 0.2 * kRate).has_value());
    const auto folder = p.session.folder();
    REQUIRE(eventually([&] {
        return ks::logs::test::find_line(ks::logs::test::read_lines(folder / "ks-engine.log"),
                                         "info", "attached a ring in slot 0")
            .has_value();
    }));
    kill_engine(p.session);
    REQUIRE(restarted(p, 2));
    REQUIRE(eventually([&] {
        std::size_t attached = 0;
        for (const std::string& line : ks::logs::test::read_lines(folder / "ks-engine.log")) {
            attached += line.find("attached a ring in slot 0") != std::string::npos ? 1 : 0;
        }
        return attached == 2;
    }));

    // The supervisor's report, in order.
    const std::vector<std::string> supervisor =
        ks::logs::test::read_lines(folder / "kool_spin.log");
    std::size_t killed = std::string::npos;
    std::size_t restarting = std::string::npos;
    std::size_t timed = std::string::npos;
    for (std::size_t i = 0; i < supervisor.size(); ++i) {
        const auto contains = [&](std::string_view text) {
            return supervisor[i].find(text) != std::string::npos;
        };
        if (contains("ks-engine was killed by signal 9")) {
            killed = i;
        } else if (contains("restarting ks-engine (restart 1)")) {
            restarting = i;
        } else if (contains("ks-engine restarted in ")) {
            timed = i;
        }
    }
    REQUIRE(killed != std::string::npos);
    REQUIRE(restarting != std::string::npos);
    REQUIRE(timed != std::string::npos);
    CHECK(killed < restarting);
    CHECK(restarting < timed);

    // The engine's own file: well formed, and the times never go back.
    const std::vector<std::string> engine = ks::logs::test::read_lines(folder / "ks-engine.log");
    CHECK(ks::logs::test::well_formed(engine, "ks-engine"));
    std::string last_time;
    for (const std::string& text : engine) {
        const auto line = ks::logs::test::parse_line(text);
        if (!line) {
            continue;
        }
        CHECK(line->time >= last_time);
        last_time = line->time;
    }
}

TEST_CASE("Chaos: killing a client, a worker, or a file leaves no gap", "[restart]") {
    const Files files;
    const std::filesystem::path sine = write_sine(files.path(), "sine.wav", 600);
    auto playing_session = start_playing(sine);
    Playing& p = *playing_session;
    p.play(0);
    REQUIRE(playing_deck(p.client, 0, 0.5 * kRate).has_value());
    // Start looking once the track has settled.
    std::this_thread::sleep_for(Milliseconds{500});
    const std::uint64_t first = p.recording.reader().frames_written();

    SECTION("a client killed with kill -9") {
        ks::processes::test::Process client =
            ks::processes::test::start(KS_PLAY_PATH, {"--runtime-dir", p.session.dir().string(),
                                                      "status", "--watch", "--interval", "50"});
        REQUIRE(client.pid() > 0);
        std::this_thread::sleep_for(Milliseconds{500});
        client.signal(SIGKILL);
        static_cast<void>(client.wait());
    }

    SECTION("a decode worker killed with kill -9") {
        for (const auto& [track_id, pid] :
             ks::track_loading::test::worker_pids(p.session.kool_spin_log())) {
            static_cast<void>(track_id);
            ::kill(pid, SIGKILL);
        }
        std::this_thread::sleep_for(Milliseconds{500});
    }

    SECTION("the file deleted") {
        std::filesystem::remove(sine);
        std::this_thread::sleep_for(Milliseconds{500});
    }

    // 2 more seconds of playing, and then the whole recording since `first`.
    const std::uint64_t at_event = p.recording.reader().frames_written();
    std::this_thread::sleep_for(Milliseconds{2'000});
    const std::uint64_t last = p.recording.reader().frames_written();
    CHECK(last >= at_event + static_cast<std::uint64_t>(1.5 * kRate));
    CHECK(p.recording.reader().starts() == 1);
    const std::vector<float> samples = recorded_from(p.recording, first, last - first);
    REQUIRE(samples.size() == last - first);
    const GapCheck check = check_no_gap(samples, 500);
    CAPTURE(check.crossings, check.widest, check.narrowest);
    CHECK(check.ok);
}

TEST_CASE("Restart: descriptors and processes don't leak", "[restart]") {
#ifdef __linux__
    const Files files;
    const std::filesystem::path constant = write_constant(files.path(), "constant.wav", 600);
    auto playing_session = start_playing(constant);
    Playing& p = *playing_session;
    p.play(0);
    REQUIRE(playing_deck(p.client, 0, 0.2 * kRate).has_value());
    const pid_t supervisor = p.session.process().pid();
    const pid_t first_engine = p.session.engine();
    const auto before = require(open_descriptors(supervisor));

    std::vector<pid_t> dead{first_engine};
    for (std::uint64_t kill = 1; kill <= 2; ++kill) {
        const auto [pid, when] = kill_engine(p.session);
        static_cast<void>(when);
        dead.push_back(pid);
        REQUIRE(restarted(p, kill + 1));
        REQUIRE(playing_deck(p.client, 0).has_value());
        // The old engine's socket is closed, and the new one's is open: the
        // count is what it was.
        CHECK(eventually([&] { return open_descriptors(supervisor) == before; }));
    }

    // Stop everything: nothing of the run is left.
    const pid_t last_engine = p.session.engine();
    const auto answer = ask(p.client, msg::Stop{});
    CHECK(ks::processes::test::is<msg::Done>(answer, p.client.next_request - 1));
    CHECK(p.session.process().wait(Milliseconds{5'000}) == 0);
    for (const pid_t pid : dead) {
        CHECK_FALSE(ks::processes::test::exists(pid));
    }
    CHECK_FALSE(ks::processes::test::exists(last_engine));
    CHECK(p.session.engine() == 0);
#else
    SKIP("this test reads /proc");
#endif
}
