// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/session-journal/spec.md requirements 8 to 16, 30,
// and 31: kool_spin writing the session journal.

#include <ks/ipc/command.h>
#include <ks/ipc/state.h>
#include <ks/supervisor/journal.h>

#include "journal_write_support.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ks::session_journal::test;
namespace fs = std::filesystem;
using ks::ipc::CommandType;
using Ms = std::chrono::milliseconds;

constexpr Ms kInterval{50};

/// A file's path, size, and modification time, as an entry should have them.
struct FileFacts {
    std::string path;
    std::uint64_t size = 0;
    std::int64_t modified = 0;
};

FileFacts facts(const fs::path& file) {
    const FileStatus status = restart::require(status_of(file));
    return FileFacts{.path = file.string(), .size = status.size, .modified = status.modified};
}

/// Whether `entry` is `file`'s, at the engine's sample rate.
bool same_file(const ks::supervisor::JournalEntry& entry, const FileFacts& file) {
    return entry.path == file.path && entry.size == file.size && entry.modified == file.modified &&
           entry.sample_rate == restart::kRate;
}

/// The engine's process ID, as an engine holds it in kool_spin's runtime
/// directory.
std::int32_t engine_pid(const restart::Session& session) {
    return static_cast<std::int32_t>(session.engine());
}

/// Sends `type`, whose argument is a code rather than a real (a crossfader
/// side, or a cue that is on or off), through `session`'s ring.
void send_code(Playing& session, CommandType type, std::uint32_t target, std::uint64_t code) {
    ks::ipc::CommandSlot slot = ks::client_rings::test::command(type, 0.0, session.sequence++);
    slot.target = target;
    slot.payload[0] = code;
    REQUIRE(session.ring.ring.push(slot));
}

/// A deck block's position.
double position_of(const ks::ipc::DeckState& block) {
    return restart::real_of(block.position);
}

} // namespace

TEST_CASE("Journal write: what it records", "[journal][write]") {
    const Files files;
    const Folder state;
    const fs::path first = restart::write_sine(files.path(), "first track.wav", 120);
    const fs::path second = restart::write_constant(files.path(), "second.wav", 60);
    auto session = restart::start_playing(first, journaled(state.path(), kInterval));
    Playing& p = *session;
    REQUIRE(restart::is_loaded(restart::load(p.client, 2, second)));
    REQUIRE(eventually([&] {
        const auto deck = p.client.state.deck(2);
        return deck && restart::loaded(*deck);
    }));

    p.send(CommandType::SetRate, 0, 1.05);
    p.send(CommandType::SetCuePoint, 0, 44'100.0);
    p.send(CommandType::Seek, 2, 12.5 * restart::kRate);
    p.send(CommandType::SetTrim, 1, -6.0);
    p.send(CommandType::SetFader, 0, 0.5);
    send_code(p, CommandType::SetCrossfaderAssign, 1, 1); // B
    send_code(p, CommandType::SetChannelCue, 0, 1);
    p.send(CommandType::SetCrossfader, 0, 0.3);
    p.send(CommandType::SetCrossfaderCurve, 0, 0.5);
    p.send(CommandType::SetMainLevel, 0, -3.0);
    p.send(CommandType::SetHeadphoneLevel, 0, -10.0);
    p.send(CommandType::SetCueMix, 0, 0.25);
    p.play(0);
    REQUIRE(applied(p));
    REQUIRE(playing_deck(p.client.state, 0).has_value());

    const FileFacts first_file = facts(first);
    const FileFacts second_file = facts(second);
    const std::optional<std::string> boot = machine_boot_id();
    std::optional<Journal> last;
    const auto matches = [&] {
        last = read_journal(state.path());
        const auto deck0 = p.client.state.deck(0);
        const auto deck2 = p.client.state.deck(2);
        const auto mixer = p.client.state.mixer();
        if (!last || !deck0 || !deck2 || !mixer) {
            return false;
        }
        const Journal& j = *last;
        if (!j.decks[0] || !j.decks[2] || j.decks[1] || j.decks[3]) {
            return false;
        }
        const auto& one = *j.decks[0];
        const auto& three = *j.decks[2];
        // Deck 1 plays: where its block puts it at the journal's time, within
        // two periods of the engine's.
        const double expected =
            restart::position_at(*deck0, static_cast<std::uint64_t>(j.time), 1.05);
        const double tolerance = (2.0 * restart::kPeriod * 1.05) + 1.0;
        return same_file(one, first_file) && one.playing && one.rate == 1.05 &&
               one.cue == restart::real_of(deck0->cue) && one.cue == 44'100.0 &&
               std::abs(one.position - expected) <= tolerance && same_file(three, second_file) &&
               !three.playing && three.rate == 1.0 && three.position == position_of(*deck2) &&
               three.cue == 0.0 && j.mixer == mixer_of(*mixer) &&
               j.engine == engine_pid(p.session) && j.boot == boot;
    };
    const bool recorded = eventually(matches, Ms{200} * kSlack);
    CAPTURE(contents(journal_path(state.path())).value_or("(none)"));
    CHECK(recorded);
    const Journal journal = restart::require(last);
    CHECK(restart::require(journal.decks[2]).position == 12.5 * restart::kRate);
    CHECK(journal.mixer.channels[1].trim_db == -6.0);
    CHECK(journal.mixer.channels[1].side == JournalSide::B);
    CHECK(journal.mixer.channels[0].cue);
    CHECK(journal.mixer.crossfader == 0.3);
    CHECK(restart::require(status_of(journal_path(state.path()))).mode == 0600U);
    CHECK_FALSE(fs::exists(state.path() / ks::supervisor::kJournalTempName));
}

TEST_CASE("Journal write: only when changed", "[journal][write]") {
    const Files files;
    const Folder state;
    constexpr Ms kSlow{200};
    auto session = restart::start_playing(
        restart::write_constant(files.path(), "constant.wav", 120), journaled(state.path(), kSlow));
    Playing& p = *session;

    // Paused: written once, then left alone.
    REQUIRE(eventually([&] { return read_journal(state.path()).has_value(); }));
    std::this_thread::sleep_for(2 * kSlow); // anything still settling
    const std::optional<FileStatus> before = status_of(journal_path(state.path()));
    REQUIRE(before.has_value());
    std::this_thread::sleep_for(10 * kSlow);
    CHECK(status_of(journal_path(state.path())) == before);

    // A seek is written within 2 intervals.
    p.send(CommandType::Seek, 0, 1'000.0);
    REQUIRE(applied(p));
    CHECK(eventually(
        [&] {
            const std::optional<Journal> journal = read_journal(state.path());
            return journal && journal->decks[0] && journal->decks[0]->position == 1'000.0;
        },
        2 * kSlow * kSlack));

    // Playing: rewritten at least every 2 intervals.
    p.play(0);
    REQUIRE(playing_deck(p.client.state, 0, 1'000.0).has_value());
    std::optional<std::string> seen = contents(journal_path(state.path()));
    auto changed_at = restart::Clock::now();
    Ms longest{0};
    const auto until = restart::Clock::now() + Ms{3'000};
    while (restart::Clock::now() < until) {
        std::this_thread::sleep_for(Ms{10});
        const std::optional<std::string> now = contents(journal_path(state.path()));
        const auto at = restart::Clock::now();
        if (now != seen) {
            seen = now;
            longest = std::max(longest, std::chrono::duration_cast<Ms>(at - changed_at));
            changed_at = at;
        }
    }
    longest = std::max(longest, std::chrono::duration_cast<Ms>(restart::Clock::now() - changed_at));
    CAPTURE(longest.count());
    CHECK(longest <= 2 * kSlow * kSlack);
}

TEST_CASE("Journal write: not before the first block", "[journal][write]") {
    const Files files;
    const Folder dir;
    const Folder state;
    const std::string before = sample_text();
    write_text(journal_path(state.path()), before);

    SECTION("nor while an engine restarts") {
        // Every engine takes 1.5 s to start: a killed one leaves the
        // journal alone until its replacement has rendered.
        restart::SessionOptions options = journaled(state.path(), kInterval);
        options.engine_options = {"--start-delay", "1500"};
        auto session =
            restart::start_playing(restart::write_constant(files.path(), "c.wav", 120), options);
        Playing& p = *session;
        p.play(0);
        REQUIRE(playing_deck(p.client.state, 0).has_value());
        const std::int32_t old = engine_pid(p.session);
        REQUIRE(eventually([&] {
            const std::optional<Journal> journal = read_journal(state.path());
            return journal && journal->engine == old;
        }));
        const auto [killed, when] = restart::kill_engine(p.session);
        static_cast<void>(when);
        REQUIRE(killed == old);
        // A write under way when it died lands by now.
        std::this_thread::sleep_for(4 * kInterval);
        const std::optional<std::string> during = contents(journal_path(state.path()));
        CHECK(restart::require(read_journal(state.path())).engine == old);
        std::this_thread::sleep_for(Ms{800});
        CHECK(contents(journal_path(state.path())) == during);
        // Once the new engine renders, the journal names it.
        REQUIRE(p.session.engine_replaced(old));
        CHECK(eventually([&] {
            const std::optional<Journal> journal = read_journal(state.path());
            return journal && journal->engine == engine_pid(p.session);
        }));
    }
    SECTION("an engine that never says Ready") {
        std::vector<std::string> args = launch_args(
            dir.path(),
            with_state(state.path(), {"--journal-interval", "50", "--ready-timeout", "500"}));
        args.insert(args.end(), {"--start-delay", "3000"});
        Launch launch{files.path(), args};
        CHECK(launch.process().wait(kLaunchWait) == 1);
        CAPTURE(launch.err());
        CHECK(contents(journal_path(state.path())) == before);
    }
    SECTION("an engine that exits at once") {
        Launch launch{files.path(),
                      launch_args(dir.path(),
                                  with_state(state.path(), {"--journal-interval", "50"}),
                                  "/usr/bin/false")};
        CHECK(launch.process().wait(kLaunchWait) == 1);
        CAPTURE(launch.err());
        CHECK(contents(journal_path(state.path())) == before);
    }
}

TEST_CASE("Journal write: atomic", "[journal][write]") {
    SECTION("a write that fails leaves the old journal") {
        const Folder state;
        const std::string old_text = sample_text();
        write_text(journal_path(state.path()), old_text);
        const std::string new_text = old_text + "and more\n";

        const pid_t pid = ::fork();
        REQUIRE(pid != -1);
        if (pid == 0) {
            static_cast<void>(std::signal(SIGXFSZ, SIG_IGN));
            const struct rlimit none{.rlim_cur = 0, .rlim_max = 0};
            if (::setrlimit(RLIMIT_FSIZE, &none) != 0) {
                ::_exit(2);
            }
            const std::expected<void, std::error_code> written =
                ks::supervisor::write_journal(state.path(), new_text);
            ::_exit(written ? 3 : 0);
        }
        ks::ipc::test::Child child{pid};
        CHECK(child.wait() == 0);
        CHECK(contents(journal_path(state.path())) == old_text);

        // Without the limit, the same write succeeds.
        REQUIRE(ks::supervisor::write_journal(state.path(), new_text).has_value());
        CHECK(contents(journal_path(state.path())) == new_text);
        CHECK(restart::require(status_of(journal_path(state.path()))).mode == 0600U);
        CHECK_FALSE(fs::exists(state.path() / ks::supervisor::kJournalTempName));
    }

    SECTION("kill -9 never leaves a broken journal") {
        const Files files;
        const Folder state;
        const fs::path wav = restart::write_constant(files.path(), "constant.wav", 60);
        std::mt19937 random{7};
        std::uniform_int_distribution<int> delay{0, 100};
        for (int i = 0; i < 20; ++i) {
            CAPTURE(i);
            auto session = restart::start_playing(wav, journaled(state.path(), Ms{10}));
            session->play(0);
            REQUIRE(playing_deck(session->client.state, 0).has_value());
            REQUIRE(eventually([&] {
                const std::optional<Journal> journal = read_journal(state.path());
                return journal && journal->engine == engine_pid(session->session);
            }));
            std::this_thread::sleep_for(Ms{delay(random)});
            session->session.process().signal(SIGKILL);
            REQUIRE(session->session.process().wait() == 128 + SIGKILL);
            CAPTURE(contents(journal_path(state.path())).value_or("(none)"));
            CHECK(read_journal(state.path()).has_value());
            // The session's reaper stops its orphaned engine.
            session.reset();
        }
    }
}

TEST_CASE("Journal write: a folder that can't be written", "[journal][write]") {
    skip_as_root();
    const Files files;
    const Folder state;
    auto session = restart::start_playing(restart::write_sine(files.path(), "sine.wav", 600),
                                          journaled(state.path(), kInterval));
    Playing& p = *session;
    p.play(0);
    REQUIRE(playing_deck(p.client.state, 0, 0.5 * restart::kRate).has_value());
    REQUIRE(eventually([&] { return read_journal(state.path()).has_value(); }));
    const std::string journal = journal_path(state.path()).string();

    {
        const Mode read_only{state.path(), 0500};
        const std::uint64_t first = p.recording.reader().frames_written();
        std::this_thread::sleep_for(20 * kInterval);
        CHECK(lines_with(p.session, "warning", journal) == 1);
        CHECK(has(p.session.stderr_text(), journal));
        const std::uint64_t last = p.recording.reader().frames_written();
        // 1 s of a 440 Hz sine: about 440 crossings.
        const restart::GapCheck gap =
            restart::check_no_gap(restart::recorded_from(p.recording, first, last - first), 400);
        CAPTURE(gap.crossings, gap.widest, gap.narrowest);
        CHECK(gap.ok);

        // An engine killed now restarts, and the deck plays on.
        const auto [old, when] = restart::kill_engine(p.session);
        static_cast<void>(when);
        REQUIRE(p.session.engine_replaced(old));
        CHECK(eventually([&] { return keeps_beating(p.client.state); }));
        CHECK(playing_deck(p.client.state, 0).has_value());
        std::this_thread::sleep_for(10 * kInterval);
        CHECK(lines_with(p.session, "warning", journal) == 1);

        // Then nothing changes: only trying again can save the session.
        p.play(0, false);
        p.send(CommandType::Seek, 0, 1'234.0);
        REQUIRE(applied(p));
        REQUIRE(eventually([&] {
            const auto deck = p.client.state.deck(0);
            return deck && !restart::playing(*deck) && position_of(*deck) == 1'234.0;
        }));
        std::this_thread::sleep_for(5 * kInterval);
    }

    // Writable again: written within 2 intervals, and said.
    const std::size_t infos = lines_with(p.session, "info", journal);
    CHECK(eventually(
        [&] {
            const std::optional<Journal> now = read_journal(state.path());
            return now && now->engine == engine_pid(p.session) && now->decks[0] &&
                   now->decks[0]->position == 1'234.0;
        },
        2 * kInterval * kSlack + Ms{100}));
    CHECK(eventually([&] { return lines_with(p.session, "info", journal) > infos; }));
}

TEST_CASE("Journal write: a writer that hangs", "[journal][write]") {
    const Files files;
    const Folder state;
    const fs::path wav = restart::write_constant(files.path(), "constant.wav", 600);
    auto session = restart::start_playing(
        wav, journaled(state.path(), kInterval, {"--journal-stall-after", "2"}));
    Playing& p = *session;
    p.play(0);
    REQUIRE(playing_deck(p.client.state, 0, 0.2 * restart::kRate).has_value());
    // The second write has stalled by now.
    std::this_thread::sleep_for(10 * kInterval);

    // A ring's commands apply.
    p.send(CommandType::SetRate, 0, 0.75);
    REQUIRE(applied(p));
    CHECK(eventually([&] {
        const auto deck = p.client.state.deck(0);
        return deck && restart::real_of(deck->rate) == 0.75;
    }));
    // A load succeeds.
    CHECK(restart::is_loaded(
        restart::load(p.client, 1, restart::write_constant(files.path(), "other.wav", 10))));
    // A frozen engine is killed and restarted.
    const pid_t old = p.session.engine();
    REQUIRE(::kill(old, SIGSTOP) == 0);
    REQUIRE(p.session.engine_replaced(old));
    CHECK(eventually([&] { return keeps_beating(p.client.state); }));
    // ks-play still gets answers.
    const Finished status =
        finished(files.path(), KS_PLAY_PATH, {"--runtime-dir", p.session.dir().string(), "status"});
    CAPTURE(status.out);
    CHECK(status.status == 0);

    const auto stopping = restart::Clock::now();
    p.session.process().signal(SIGTERM);
    const Ms limit = kSlowSanitizer ? Ms{3'500} : Ms{2'500};
    CHECK(p.session.process().wait(limit) == 0);
    CAPTURE(std::chrono::duration_cast<Ms>(restart::Clock::now() - stopping).count());
    CAPTURE(p.session.stderr_text());
    CHECK(has(p.session.stderr_text(), journal_path(state.path()).string()));
}

TEST_CASE("Journal write: without the lock", "[journal][write]") {
    skip_as_root();
    const Files files;
    const Folder state;
    const std::string before = sample_text();
    write_text(journal_path(state.path()), before);
    const fs::path lock = lock_path(state.path(), ks::ipc::kSupervisorLockName);
    write_text(lock, "");
    const Mode read_only{lock, 0400};

    auto session = restart::start_playing(restart::write_constant(files.path(), "c.wav", 120),
                                          journaled(state.path(), kInterval));
    Playing& p = *session;
    CAPTURE(p.session.stderr_text());
    CHECK(has(p.session.stderr_text(), "won't be saved"));
    p.play(0);
    REQUIRE(playing_deck(p.client.state, 0, 0.2 * restart::kRate).has_value());
    std::this_thread::sleep_for(10 * kInterval);
    CHECK(contents(journal_path(state.path())) == before);
}

TEST_CASE("Journal write: the last write", "[journal][write]") {
    const Files files;
    const Folder state;
    auto session = restart::start_playing(restart::write_constant(files.path(), "c.wav", 120),
                                          journaled(state.path(), Ms{60'000}));
    Playing& p = *session;
    // Its first block, so that a journal may be written.
    REQUIRE(eventually([&] { return p.client.state.heartbeat() > 0; }));
    p.send(CommandType::Seek, 0, 3.0 * restart::kRate);
    REQUIRE(applied(p));
    REQUIRE(eventually([&] {
        const auto deck = p.client.state.deck(0);
        return deck && position_of(*deck) == 3.0 * restart::kRate;
    }));
    std::this_thread::sleep_for(Ms{10});
    p.session.process().signal(SIGTERM);
    CHECK(p.session.process().wait(kLaunchWait) == 0);

    CAPTURE(contents(journal_path(state.path())).value_or("(none)"));
    CAPTURE(contents(p.session.kool_spin_log()).value_or("(no log)"));
    const ks::supervisor::JournalEntry deck =
        restart::require(restart::require(read_journal(state.path())).decks[0]);
    CHECK(deck.position == 3.0 * restart::kRate);
    CHECK_FALSE(deck.playing);
}

TEST_CASE("Journal write: no state folder", "[journal][write]") {
    const Files files;
    const Folder dir;
    const fs::path home = files.path() / "home";
    make_private(home);
    const std::vector<std::string> args = launch_args(dir.path(), {"--journal-interval", "50"});
    Running running{files.path(),
                    KS_KOOL_SPIN_PATH,
                    args,
                    {"HOME=" + home.string(), "XDG_STATE_HOME=" + (home / "state").string()}};
    REQUIRE(new_session(dir.path(), 0));

    std::expected<restart::Client, std::string> client = restart::join(dir.path(), "player");
    REQUIRE(client.has_value());
    REQUIRE(restart::is_loaded(
        restart::load(*client, 0, restart::write_constant(files.path(), "c.wav", 60))));
    auto ring = restart::open_ring(*client);
    REQUIRE(ring.ring.push(ks::track_loading::test::set_playing(0, true, 1)));
    REQUIRE(playing_deck(client->state, 0).has_value());
    std::this_thread::sleep_for(10 * kInterval);

    CHECK(files_in(home).empty());
    CHECK_FALSE(fs::exists(dir.path() / ks::supervisor::kJournalName));
    CHECK(stop(dir.path()));
    CHECK(running.process().wait(kLaunchWait) == 0);
    CHECK(files_in(home).empty());
}

TEST_CASE("Journal write: options", "[journal][options]") {
    const Files files;
    const Folder dir;
    const auto run = [&](std::vector<std::string> options) {
        options.insert(options.begin(), {"--runtime-dir", dir.path().string()});
        return finished(files.path(), KS_KOOL_SPIN_PATH, options);
    };
    for (const std::vector<std::string>& bad : std::vector<std::vector<std::string>>{
             {"--journal-interval", "50", "--journal-interval", "50"},
             {"--journal-interval", "0"},
             {"--journal-interval", "3600001"},
             {"--journal-interval", "x"},
             {"--journal-interval"},
             {"--journal-stall-after", "0"},
             {"--journal-stall-after", "1001"},
             {"--journal-stall-after", "1", "--journal-stall-after", "1"}}) {
        const Finished said = run(bad);
        CAPTURE(bad, said.out);
        CHECK(said.status == 2);
        CHECK(has(said.out, "kool_spin: "));
    }
    const Finished help = run({"--help"});
    CHECK(help.status == 0);
    CAPTURE(help.out);
    CHECK(has(help.out, "--journal-interval"));
    CHECK_FALSE(has(help.out, "--journal-stall-after"));
}
