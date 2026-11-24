// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/hang-detection/spec.md: kool_spin killing an
// engine whose heartbeat stops, and dealing with one whose control thread
// stops answering pings. Every test starts kool_spin, so none can run in the
// build loop's sandbox; the maintainer runs them outside it.
//
// Two ways to hang an engine. SIGSTOP freezes the whole process: the heartbeat
// and the Pongs both stop. `ks-engine --hang-control-after <ms>` stops only
// the control thread. (SIGSTOP can't freeze one thread, so the "late answer"
// tests lengthen --hang-timeout to keep the heartbeat watch out of the way.)

#include <ks/ipc/command.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/recording.h>
#include <ks/ipc/state.h>

#include "restart_support.h"
#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace ks::engine_restart::test;
using ks::ipc::CommandType;
namespace msg = ks::ipc::msg;

/// A temporary folder for the audio files, apart from the session's own.
using Files = ks::ipc::test::TempDir;

/// How long the detection may take with the defaults (spec: within 1.5 s; a
/// sanitizer's start-up and scheduling get 4 s).
constexpr Milliseconds kDetectionLimit = kSlowSanitizer ? Milliseconds{4'000} : Milliseconds{1'500};

/// The deck block of deck `deck`, once it's playing: within 5 s.
std::optional<ks::ipc::DeckState> playing_deck(const Client& client, std::size_t deck,
                                               double past_frames = 0.0) {
    std::optional<ks::ipc::DeckState> last;
    const auto ready = [&] {
        last = client.state.deck(deck);
        return last && loaded(*last) && playing(*last) && real_of(last->position) > past_frames;
    };
    static_cast<void>(eventually(ready));
    return ready() ? last : std::nullopt;
}

/// True if, within `timeout`, the engine has been started `starts` times on the
/// recording, and the newest start has recorded a few periods.
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

ks::ipc::CommandSlot deck_command(CommandType type, std::uint32_t deck, double value,
                                  std::uint64_t sequence) {
    ks::ipc::CommandSlot command = ks::client_rings::test::command(type, value, sequence);
    command.target = deck;
    return command;
}

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

/// How many lines of the supervisor's log contain `phrase`, at any level.
std::size_t log_count(const Session& session, std::string_view phrase) {
    std::size_t count = 0;
    for (const std::string& line : ks::logs::test::read_lines(session.kool_spin_log())) {
        count += line.find(phrase) != std::string::npos ? 1 : 0;
    }
    return count;
}

/// True if, within `timeout`, the supervisor's log has a line with `phrase`.
bool log_says(const Session& session, std::string_view phrase,
              Milliseconds timeout = Milliseconds{10'000}) {
    return eventually([&] { return log_count(session, phrase) > 0; }, timeout);
}

/// Freezes `pid`: the heartbeat and every Pong stop.
void freeze(pid_t pid) {
    REQUIRE(::kill(pid, SIGSTOP) == 0);
}
void thaw(pid_t pid) {
    REQUIRE(::kill(pid, SIGCONT) == 0);
}

/// Options for a session that has a recording and the given supervisor
/// options.
SessionOptions with_supervisor(std::vector<std::string> supervisor,
                               std::vector<std::string> engine_options = {}) {
    SessionOptions options;
    options.supervisor = std::move(supervisor);
    options.engine_options = std::move(engine_options);
    return options;
}

/// A session that is playing a long constant track on deck 0.
std::unique_ptr<Playing> playing_constant(const Files& files, const SessionOptions& options = {}) {
    const std::filesystem::path wav = write_constant(files.path(), "constant.wav", 600);
    auto playing_session = start_playing(wav, options);
    playing_session->play(0);
    REQUIRE(playing_deck(playing_session->client, 0, 0.2 * kRate).has_value());
    return playing_session;
}

} // namespace

TEST_CASE("Hang: a stalled heartbeat is killed and the deck resumes", "[hang]") {
    const Files files;
    const std::filesystem::path wav = write_constant(files.path(), "constant.wav", 600);
    auto playing_session = start_playing(wav);
    Playing& p = *playing_session;
    p.play(0);
    const auto first = require(playing_deck(p.client, 0, 0.5 * kRate));
    const std::uint64_t track_id = first.track_id;

    const auto before = require(p.client.state.deck(0));
    const std::uint64_t written_before = p.recording.reader().frames_written();
    REQUIRE(written_before > 4'000);
    const double steady = mean(require(p.recording.left(written_before - 2'000, 1'000)));
    REQUIRE(steady > 0.1);

    const pid_t old_pid = p.session.engine();
    REQUIRE(old_pid > 0);
    const Clock::time_point stopped_at = Clock::now();
    freeze(old_pid);

    // The supervisor kills it: the old process is gone within the limit.
    REQUIRE(eventually([&] { return !ks::processes::test::exists(old_pid); }, kRestartWait));
    const Milliseconds detection =
        std::chrono::duration_cast<Milliseconds>(Clock::now() - stopped_at);
    std::cout << "hang detection time: " << detection.count() << " ms\n";
    CHECK(detection <= kDetectionLimit);

    REQUIRE(restarted(p, 2));
    CHECK(p.session.engine() != old_pid);
    CHECK(p.session.process().running());
    CHECK(log_says(p.session, "ks-engine heartbeat stopped for "));
    CHECK(log_says(p.session, "ks-engine was killed by signal 9"));
    CHECK(log_says(p.session, "ks-engine restarted in "));

    // The deck: the same track, playing, where it would be.
    const auto after = require(playing_deck(p.client, 0));
    CHECK(after.track_id == track_id);
    const double expected = position_at(before, after.host_time);
    CAPTURE(expected, real_of(after.position));
    CHECK(std::abs(real_of(after.position) - expected) < 0.010 * kRate);

    // The recording: one new boundary, after what the old engine had written,
    // and a fade-in from silence that never drops.
    const auto boundaries = p.recording.reader().boundaries();
    REQUIRE(boundaries.size() == 2);
    CHECK(boundaries[1].frame >= written_before);
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
}

TEST_CASE("Hang: decks, mixer, cue point, and rings come back", "[hang]") {
    const Files files;
    const std::filesystem::path constant = write_constant(files.path(), "constant.wav", 600);
    const std::filesystem::path sine = write_sine(files.path(), "sine.wav", 600);
    auto playing_session = start_playing(constant);
    Playing& p = *playing_session;
    REQUIRE(is_loaded(load(p.client, 1, sine)));

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
    CHECK(real_of(require(p.client.state.mixer()).crossfader) == 0.5);

    const pid_t old_pid = p.session.engine();
    freeze(old_pid);
    // A command written while the engine is hung and then restarted.
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

    CHECK(applied(p.client.state, 1, 1));
    CHECK(real_of(require(p.client.state.mixer()).crossfader) == 0.25);
    p.send(CommandType::SetCrossfader, 0, 0.75);
    CHECK(applied(p.client.state, 0, p.sequence - 1));
}

TEST_CASE("Hang: a healthy engine is never killed", "[hang]") {
    const Files files;
    const auto watch = [](Session& session) {
        const pid_t pid = session.engine();
        REQUIRE(pid > 0);
        std::this_thread::sleep_for(Milliseconds{5'000});
        CHECK(session.engine() == pid);
        CHECK(session.process().running());
        CHECK(log_count(session, "heartbeat stopped") == 0);
        CHECK(log_count(session, "restarting ks-engine") == 0);
    };

    SECTION("a deck playing") {
        auto playing_session = playing_constant(files, with_supervisor({"--hang-timeout", "500"}));
        watch(playing_session->session);
    }
    SECTION("every deck empty") {
        Session session{with_supervisor({"--hang-timeout", "500"})};
        watch(session);
    }
    SECTION("every deck paused") {
        auto playing_session = start_playing(write_constant(files.path(), "paused.wav", 600),
                                             with_supervisor({"--hang-timeout", "500"}));
        watch(playing_session->session);
    }
}

TEST_CASE("Hang: a short stall is not a hang", "[hang]") {
    const Files files;
    auto playing_session = playing_constant(files);
    const Playing& p = *playing_session;
    const pid_t pid = p.session.engine();
    freeze(pid);
    std::this_thread::sleep_for(Milliseconds{300});
    thaw(pid);
    std::this_thread::sleep_for(Milliseconds{2'000});
    CHECK(p.session.engine() == pid);
    CHECK(log_count(p.session, "heartbeat stopped") == 0);
    CHECK(playing_deck(p.client, 0).has_value());
}

TEST_CASE("Hang: a starting engine is not watched", "[hang]") {
    // Start-up takes 1.5 s: three times the hang timeout. The engine isn't
    // sending a heartbeat yet, and mustn't be killed for it.
    const SessionOptions options = with_supervisor(
        {"--hang-timeout", "500", "--ready-timeout", "10000"}, {"--start-delay", "1500"});
    Session session{options};
    REQUIRE(eventually([&] { return session.engine() > 0; }));
    const pid_t starting = session.engine();
    std::expected<Client, std::string> client = std::unexpected{std::string{"not yet"}};
    REQUIRE(eventually(
        [&] {
            client = join(session.dir(), "late");
            return client.has_value();
        },
        Milliseconds{10'000}));
    // Ready has come, and the engine is the one that started.
    CHECK(session.engine() == starting);
    CHECK(eventually([&] { return client->state.heartbeat() > 0; }));
    CHECK(log_count(session, "heartbeat stopped") == 0);
    CHECK(log_count(session, "restarting ks-engine") == 0);
}

namespace {
/// How long the guard test waits for the engine to be killed three or four
/// times, each start taking seconds on a loaded runner under a sanitizer.
constexpr Milliseconds kGuardWait = kSlowSanitizer ? Milliseconds{60'000} : Milliseconds{20'000};
} // namespace

TEST_CASE("Hang: kills count toward the crash-loop guard", "[hang]") {
    const Files files;
    auto playing_session = playing_constant(files);
    Playing& p = *playing_session;

    // The first two hangs restart as usual: the deck resumes.
    for (std::uint64_t hang = 1; hang <= 2; ++hang) {
        freeze(p.session.engine());
        REQUIRE(restarted(p, hang + 1, kGuardWait));
        REQUIRE(playing_deck(p.client, 0).has_value());
    }

    // The third is a clean restart: no track, nothing playing, and the ring's
    // slot is still attached and works.
    freeze(p.session.engine());
    REQUIRE(restarted(p, 4, kGuardWait));
    CHECK(eventually([&] {
        const auto deck = p.client.state.deck(0);
        return deck && !loaded(*deck) && !playing(*deck);
    }));
    p.send(CommandType::SetCrossfader, 0, 0.25);
    CHECK(applied(p.client.state, 0, p.sequence - 1));

    // The fourth gives up: status 3, and the guard's message.
    freeze(p.session.engine());
    CHECK(p.session.process().wait(kGuardWait) == 3);
    CHECK(p.session.stderr_text().find("ks-engine keeps failing; not restarting it") !=
          std::string::npos);
    CHECK(p.session.engine() == 0);
}

TEST_CASE("Hang: an unresponsive control thread with a playing deck is left running", "[hang]") {
    // The control thread stops 4 s after Ready (after the setup, which needs
    // it). The pings go unanswered for 3 x 200 ms, and the supervisor reports
    // it, once, and leaves the engine alone while the deck plays.
    const Files files;
    const std::filesystem::path sine = write_sine(files.path(), "sine.wav", 600);
    auto playing_session = start_playing(
        sine, with_supervisor({"--ping-interval", "200"}, {"--hang-control-after", "4000"}));
    Playing& p = *playing_session;
    const pid_t pid = p.session.engine();
    p.play(0);
    REQUIRE(playing_deck(p.client, 0, 0.2 * kRate).has_value());

    REQUIRE(log_says(p.session, "ks-engine control thread not answering", Milliseconds{20'000}));
    const std::uint64_t first = p.recording.reader().frames_written();
    // Three more ping limits, with the deck playing the whole time.
    std::this_thread::sleep_for(Milliseconds{(3 * 3 * 200) + 1'000});
    const std::uint64_t last = p.recording.reader().frames_written();
    CHECK(p.session.engine() == pid);
    CHECK(log_count(p.session, "control thread not answering") == 1);
    CHECK(log_count(p.session, "restarting ks-engine") == 0);
    CHECK(p.session.process().running());

    // The audio carried on without a gap.
    REQUIRE(last > first);
    const std::vector<float> samples = recorded_from(p.recording, first, last - first);
    REQUIRE(samples.size() == last - first);
    const GapCheck check = check_no_gap(samples, 200);
    CAPTURE(check.crossings, check.widest, check.narrowest);
    CHECK(check.ok);
    CHECK(p.recording.reader().starts() == 1);

    // The supervisor still answers its clients.
    // (ask() skips notifications, such as the DecodeEnded that is waiting.)
    const auto pong = ask(p.client, msg::Ping{});
    CHECK(ks::processes::test::is<msg::Pong>(pong, p.client.next_request - 1));
}

TEST_CASE("Hang: an unresponsive control thread is killed once no deck plays", "[hang]") {
    const Files files;
    const std::filesystem::path constant = write_constant(files.path(), "constant.wav", 600);
    const SessionOptions options =
        with_supervisor({"--ping-interval", "200"}, {"--hang-control-after", "4000"});

    SECTION("no deck was playing") {
        // Loaded, but paused throughout.
        auto playing_session = start_playing(constant, options);
        const Playing& p = *playing_session;
        const pid_t old_pid = p.session.engine();
        CHECK(p.session.engine_replaced(old_pid, Milliseconds{30'000}));
        CHECK(log_says(p.session, "no deck is playing; killing it"));
        CHECK(log_says(p.session, "ks-engine was killed by signal 9"));
    }

    SECTION("the deck is paused after the engine became unresponsive") {
        auto playing_session = start_playing(constant, options);
        Playing& p = *playing_session;
        const pid_t old_pid = p.session.engine();
        p.play(0);
        REQUIRE(playing_deck(p.client, 0, 0.2 * kRate).has_value());
        REQUIRE(
            log_says(p.session, "ks-engine control thread not answering", Milliseconds{20'000}));
        // Playing: still the same engine, and no kill.
        std::this_thread::sleep_for(Milliseconds{1'000});
        REQUIRE(p.session.engine() == old_pid);
        CHECK(log_count(p.session, "no deck is playing; killing it") == 0);

        // The pause goes through the ring, which the hung control thread isn't
        // needed for. The kill follows within a poll or two of it being
        // applied.
        p.play(0, false);
        const Clock::time_point paused_at = Clock::now();
        REQUIRE(p.session.engine_replaced(old_pid, Milliseconds{5'000}));
        std::cout << "kill after the pause: "
                  << std::chrono::duration_cast<Milliseconds>(Clock::now() - paused_at).count()
                  << " ms\n";
        CHECK(log_says(p.session, "no deck is playing; killing it"));
    }
}

TEST_CASE("Hang: a late Pong ends the report", "[hang]") {
    // A long --hang-timeout keeps the heartbeat watch out of the way, so that
    // freezing the whole engine looks, to the pings, like a slow control
    // thread. The ping limit is 3 x 300 = 900 ms.
    const Files files;
    auto playing_session = playing_constant(
        files, with_supervisor({"--hang-timeout", "4000", "--ping-interval", "300"}));
    const Playing& p = *playing_session;
    const pid_t pid = p.session.engine();

    SECTION("an answer within the limit") {
        freeze(pid);
        std::this_thread::sleep_for(Milliseconds{450});
        thaw(pid);
        std::this_thread::sleep_for(Milliseconds{3'000});
        CHECK(log_count(p.session, "control thread not answering") == 0);
        CHECK(p.session.engine() == pid);
    }

    SECTION("an answer after the limit, with a deck playing") {
        freeze(pid);
        std::this_thread::sleep_for(Milliseconds{2'200});
        thaw(pid);
        CHECK(log_says(p.session, "ks-engine control thread answering again"));
        CHECK(log_count(p.session, "control thread not answering") == 1);
        CHECK(p.session.engine() == pid);
        CHECK(log_count(p.session, "restarting ks-engine") == 0);
        CHECK(playing_deck(p.client, 0).has_value());
    }
}

TEST_CASE("Hang: ping state is reset at each Ready", "[hang]") {
    // An engine whose control thread hangs is reported; the same engine's
    // replacement, which hangs the same way later, must be reported on its own
    // account, and a healthy replacement must not be reported at all because
    // of what its predecessor failed to answer. (The supervisor logs no pings,
    // so what a test can see is the report: none within one ping limit of the
    // new engine's Ready.)
    const Files files;
    auto playing_session = playing_constant(
        files, with_supervisor({"--hang-timeout", "4000", "--ping-interval", "300"}));
    const Playing& p = *playing_session;
    const pid_t old_pid = p.session.engine();

    // Leave a ping unanswered, and end the engine in the middle of it.
    freeze(old_pid);
    std::this_thread::sleep_for(Milliseconds{500});
    REQUIRE(::kill(old_pid, SIGKILL) == 0);
    REQUIRE(restarted(p, 2, kGuardWait));

    // The new engine is healthy: for twice the limit, nothing is reported.
    std::this_thread::sleep_for(Milliseconds{2 * 3 * 300});
    CHECK(log_count(p.session, "control thread not answering") == 0);
    CHECK(p.session.engine() != old_pid);
}

TEST_CASE("Hang: options", "[hang]") {
    const auto refused = [](const std::vector<std::string>& args) {
        ks::processes::test::Process process =
            ks::processes::test::start(ks::processes::test::kKoolSpinPath, args);
        return process.pid() > 0 && process.wait() == 2;
    };
    CHECK(refused({"--hang-timeout", "0"}));
    CHECK(refused({"--hang-timeout", "abc"}));
    CHECK(refused({"--hang-timeout"}));
    CHECK(refused({"--ping-interval", "0"}));
    CHECK(refused({"--ping-interval", "abc"}));
    CHECK(refused({"--ping-interval", "-5"}));

    SECTION("ks-engine --hang-control-after needs the paced backend") {
        // Parsing comes first, so the descriptors can be anything open.
        const auto engine_refuses = [&](const std::vector<std::string>& more) {
            std::vector<std::string> args{"--supervisor-fd", "3",           "--state-fd", "4",
                                          "--runtime-dir",   "/nonexistent"};
            args.insert(args.end(), more.begin(), more.end());
            const ks::ipc::Fd fd{::dup(STDIN_FILENO)};
            REQUIRE(fd.get() >= 0);
            const std::vector<ks::supervisor::Inherit> fds{{.parent = fd.get(), .child = 3},
                                                           {.parent = fd.get(), .child = 4}};
            ks::processes::test::Process engine =
                ks::processes::test::start(ks::processes::test::kEnginePath, args, fds);
            return engine.pid() > 0 && engine.wait() == 2;
        };
        CHECK(engine_refuses({"--hang-control-after", "100"})); // default backend
        CHECK(engine_refuses({"--backend", "paced", "--hang-control-after", "x"})); // not a number
        CHECK(engine_refuses({"--backend", "paced", "--hang-control-after"}));      // no value
    }

    SECTION("--help lists the options") {
        const ks::ipc::test::TempDir dir;
        const std::filesystem::path out = dir.path() / "help.txt";
        ks::processes::test::Process help = ks::processes::test::start(
            "/bin/sh", {"-c", std::string{"\""} + std::string{ks::processes::test::kKoolSpinPath} +
                                  "\" --help > \"" + out.string() + "\" 2>&1"});
        REQUIRE(help.pid() > 0);
        CHECK(help.wait() == 0);
        std::ifstream in{out};
        const std::string text{std::istreambuf_iterator<char>{in},
                               std::istreambuf_iterator<char>{}};
        CHECK(text.find("--hang-timeout") != std::string::npos);
        CHECK(text.find("--ping-interval") != std::string::npos);
    }
}

TEST_CASE("Hang: descriptors and processes don't leak", "[hang]") {
#ifdef __linux__
    const Files files;
    // A short track, so that its decoding ends (and the worker's descriptors
    // close) before the baseline is taken, even under a sanitizer.
    const std::filesystem::path wav = write_constant(files.path(), "short.wav", 30);
    auto playing_session = start_playing(wav);
    Playing& p = *playing_session;
    p.play(0);
    REQUIRE(playing_deck(p.client, 0, 0.2 * kRate).has_value());
    const pid_t supervisor = p.session.process().pid();
    // Its DecodeEnded.
    const std::optional<Received> decoded = ks::processes::test::receive(p.client.connection);
    REQUIRE(ks::processes::test::is<msg::DecodeEnded>(decoded, 0));
    // The decode worker's descriptors close when the track has been decoded:
    // the baseline is taken once the count stands still.
    std::optional<std::size_t> before;
    REQUIRE(eventually(
        [&] {
            const auto first = open_descriptors(supervisor);
            std::this_thread::sleep_for(Milliseconds{500});
            before = open_descriptors(supervisor);
            return first && first == before;
        },
        Milliseconds{30'000}));

    std::vector<pid_t> dead{p.session.engine()};
    for (std::uint64_t hang = 1; hang <= 2; ++hang) {
        freeze(p.session.engine());
        REQUIRE(restarted(p, hang + 1, kGuardWait));
        REQUIRE(playing_deck(p.client, 0).has_value());
        // The old engine's socket is closed and the new one's is open.
        const bool settled = eventually([&] { return open_descriptors(supervisor) == before; });
        CAPTURE(hang, before.value_or(0), open_descriptors(supervisor).value_or(0));
        CHECK(settled);
        dead.push_back(p.session.engine());
    }
    dead.pop_back(); // the last one is alive

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
