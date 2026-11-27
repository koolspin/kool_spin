// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/session-journal/spec.md requirements 17 to 30:
// kool_spin restoring the session from the journal at launch.

#include <ks/ipc/command.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/state.h>
#include <ks/supervisor/journal.h>

#include "journal_write_support.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace ks::session_journal::test;
namespace fs = std::filesystem;
using ks::ipc::CommandType;
using ks::supervisor::JournalEntry;
using ks::track_loading::test::FakeTrack;
using Ms = std::chrono::milliseconds;

constexpr Ms kInterval{50};
/// How far a restored deck may be from its entry after everything was
/// killed: one block (the criterion "After everything is killed").
constexpr double kBlock = 1'024.0;

/// `file`'s entry, as a journal written when the supervisor opened it would
/// hold it: its path, size, and modification time now.
JournalEntry entry_for(const fs::path& file, double position, double cue = 0.0, double rate = 1.0,
                       bool playing = false) {
    const FileStatus status = restart::require(status_of(file));
    return JournalEntry{.path = file.string(),
                        .size = status.size,
                        .modified = status.modified,
                        .sample_rate = restart::kRate,
                        .position = position,
                        .rate = rate,
                        .cue = cue,
                        .playing = playing};
}

/// `entry`, as the journal carries it: not playing (requirement 24).
JournalEntry carried(JournalEntry entry) {
    entry.playing = false;
    return entry;
}

void put_journal(const fs::path& state, const Journal& journal) {
    write_text(journal_path(state), ks::supervisor::format(journal));
}

/// Writes `<dir>/<name>`: a mono, 16-bit WAV of `frames` frames of a constant
/// 0.25 at `rate` Hz. Returns the path.
fs::path write_wav_at(const fs::path& dir, std::string_view name, std::uint64_t frames,
                      std::uint32_t rate) {
    const auto put = [](std::ofstream& out, std::uint64_t number, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            out.put(static_cast<char>((number >> (8 * i)) & 0xFFU));
        }
    };
    const fs::path path = dir / name;
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write("RIFF", 4);
    put(out, 36 + (frames * 2), 4);
    out.write("WAVEfmt ", 8);
    put(out, 16, 4);
    put(out, 1, 2); // PCM
    put(out, 1, 2); // mono
    put(out, rate, 4);
    put(out, std::uint64_t{rate} * 2, 4);
    put(out, 2, 2);
    put(out, 16, 2);
    out.write("data", 4);
    put(out, frames * 2, 4);
    for (std::uint64_t n = 0; n < frames; ++n) {
        put(out, 8'192, 2); // 0.25
    }
    out.close();
    REQUIRE(out.good());
    return path;
}

/// A launch with a client of its own.
struct Relaunch {
    restart::Session session;
    restart::Client client;

    explicit Relaunch(const restart::SessionOptions& options)
        : session{options}, client{joined(session)} {}

private:
    static restart::Client joined(const restart::Session& session) {
        std::expected<restart::Client, std::string> client = restart::join(session.dir(), "looker");
        CAPTURE(session.stderr_text());
        REQUIRE(client.has_value());
        return std::move(*client);
    }
};

/// Deck `deck`'s block, once the engine has published one; nothing if it
/// hasn't within 5 s.
std::optional<ks::ipc::DeckState> deck_block(const restart::State& state, std::size_t deck) {
    std::optional<ks::ipc::DeckState> block;
    static_cast<void>(eventually([&] {
        block = state.deck(deck);
        return block.has_value();
    }));
    return block;
}

/// Checks that deck `deck` holds `entry`'s track, paused, within `tolerance`
/// frames of its position, with its rate and cue point.
void check_restored(const restart::State& state, std::size_t deck, const JournalEntry& entry,
                    double tolerance = 0.0) {
    CAPTURE(deck, entry.path);
    const ks::ipc::DeckState block = restart::require(deck_block(state, deck));
    CHECK(restart::loaded(block));
    CHECK_FALSE(restart::playing(block));
    CHECK(block.track_id != 0);
    CHECK(block.track_sample_rate == entry.sample_rate);
    const double position = restart::real_of(block.position);
    CAPTURE(position, entry.position);
    CHECK(std::abs(position - entry.position) <= tolerance);
    CHECK(restart::real_of(block.rate) == entry.rate);
    CHECK(restart::real_of(block.cue) == entry.cue);
}

/// Checks that deck `deck` is empty.
void check_empty(const restart::State& state, std::size_t deck) {
    CAPTURE(deck);
    const ks::ipc::DeckState block = restart::require(deck_block(state, deck));
    CHECK_FALSE(restart::loaded(block));
    CHECK(block.track_id == 0);
}

/// The journal, once one that `session`'s engine wrote has rendered is on
/// disk, and `ready` accepts it; nothing if not within `timeout`.
template <typename Ready>
std::optional<Journal> journal_from(const fs::path& state, const restart::Session& session,
                                    Ready ready, Ms timeout = Ms{5'000}) {
    std::optional<Journal> last;
    const bool found = eventually(
        [&] {
            last = read_journal(state);
            return last && last->engine == static_cast<std::int32_t>(session.engine()) &&
                   ready(*last);
        },
        timeout);
    return found ? last : std::nullopt;
}

std::optional<Journal> journal_from(const fs::path& state, const restart::Session& session) {
    return journal_from(state, session, [](const Journal&) { return true; });
}

/// True if a warning in `session`'s kool_spin.log contains every one of
/// `phrases`.
bool warned(const restart::Session& session, const std::vector<std::string>& phrases) {
    return eventually([&] {
        const std::vector<std::string> lines = ks::logs::test::read_lines(session.kool_spin_log());
        return std::ranges::any_of(lines, [&](const std::string& raw) {
            const std::optional<ks::logs::test::Line> line = ks::logs::test::parse_line(raw);
            return line && line->level == "warning" &&
                   std::ranges::all_of(phrases, [&](const std::string& phrase) {
                       return line->text.find(phrase) != std::string::npos;
                   });
        });
    });
}

/// The process IDs that `session`'s kool_spin.log says it started ks-worker as.
std::vector<pid_t> worker_pids(const restart::Session& session) {
    constexpr std::string_view kStarted = "started ks-worker (process ";
    std::vector<pid_t> pids;
    for (const std::string& line : ks::logs::test::read_lines(session.kool_spin_log())) {
        const std::size_t at = line.find(kStarted);
        if (at == std::string::npos) {
            continue;
        }
        const std::string_view rest = std::string_view{line}.substr(at + kStarted.size());
        pid_t pid = 0;
        const char* const first = std::to_address(rest.begin());
        const auto [end, error] = std::from_chars(first, std::to_address(rest.end()), pid);
        if (error == std::errc{} && end != first) {
            pids.push_back(pid);
        }
    }
    return pids;
}

/// The process ID that `session`'s kool_spin.log says it started ks-worker as
/// for track `track`; 0 if it doesn't.
pid_t worker_pid(const restart::Session& session, std::uint64_t track) {
    const std::string tail = std::format(") for track {}", track);
    const std::vector<pid_t> pids = worker_pids(session);
    const std::vector<std::string> lines = ks::logs::test::read_lines(session.kool_spin_log());
    std::size_t next = 0;
    for (const std::string& line : lines) {
        if (line.find("started ks-worker (process ") == std::string::npos) {
            continue;
        }
        if (line.ends_with(tail) && next < pids.size()) {
            return pids.at(next);
        }
        ++next;
    }
    return 0;
}

/// Stops `session`'s kool_spin with SIGTERM: the last write records the
/// session (requirement 14).
void stop_cleanly(restart::Session& session) {
    session.process().signal(SIGTERM);
    CAPTURE(session.stderr_text());
    REQUIRE(session.process().wait(kLaunchWait) == 0);
}

/// Kills `session`'s kool_spin and its engine with SIGKILL, once its workers
/// have ended: nothing is left to write the journal.
void kill_everything(restart::Session& session) {
    const pid_t engine = session.engine();
    REQUIRE(engine > 0);
    const std::vector<pid_t> workers = worker_pids(session);
    session.process().signal(SIGKILL);
    REQUIRE(session.process().wait() == 128 + SIGKILL);
    REQUIRE(::kill(engine, SIGKILL) == 0);
    REQUIRE(eventually([&] { return ended(engine); }));
    for (const pid_t worker : workers) {
        CAPTURE(worker);
        REQUIRE(eventually([&] { return ended(worker); }));
    }
}

/// True once both decks' tracks are decoded, so their workers end.
bool decoded(const restart::State& state, std::size_t deck) {
    return eventually([&] {
        const auto block = state.deck(deck);
        return block && (block->flags & ks::ipc::kDeckComplete) != 0;
    });
}

/// True if the recording holds a sample above `level` from frame `first` on,
/// within 5 s.
bool sounds(const restart::Recording& recording, std::uint64_t first, float level = 0.1F) {
    return eventually([&] {
        const std::vector<float> samples = restart::recorded_from(recording, first);
        return std::ranges::any_of(samples, [level](float s) { return std::abs(s) > level; });
    });
}

} // namespace

TEST_CASE("Journal restore: after everything is killed", "[journal][restore]") {
    const Files files;
    const Folder state;
    const fs::path first = restart::write_sine(files.path(), "first.wav", 120);
    const fs::path second = restart::write_sine(files.path(), "second.wav", 60);
    Journal before;
    {
        auto session = restart::start_playing(first, journaled(state.path(), kInterval));
        Playing& p = *session;
        REQUIRE(restart::is_loaded(restart::load(p.client, 2, second)));
        p.send(CommandType::SetRate, 0, 1.05);
        p.send(CommandType::SetCuePoint, 0, 44'100.0);
        p.send(CommandType::Seek, 2, 12.5 * restart::kRate);
        p.send(CommandType::SetTrim, 2, -4.5);
        p.send(CommandType::SetFader, 0, 0.7);
        p.send(CommandType::SetCrossfader, 0, 0.25);
        p.send(CommandType::SetMainLevel, 0, -2.0);
        p.play(0);
        REQUIRE(applied(p));
        REQUIRE(decoded(p.client.state, 0));
        REQUIRE(decoded(p.client.state, 2));
        REQUIRE(playing_deck(p.client.state, 0, 0.2 * restart::kRate).has_value());
        REQUIRE(journal_from(state.path(), p.session, [&](const Journal& j) {
                    const auto mixer = p.client.state.mixer();
                    return j.decks[0] && j.decks[0]->playing && j.decks[0]->cue == 44'100.0 &&
                           j.decks[2] && j.decks[2]->position == 12.5 * restart::kRate && mixer &&
                           j.mixer == mixer_of(*mixer) && j.mixer.crossfader == 0.25;
                }).has_value());
        kill_everything(p.session);
        before = restart::require(read_journal(state.path()));
    }
    CAPTURE(ks::supervisor::format(before));
    const JournalEntry one = restart::require(before.decks[0]);
    const JournalEntry three = restart::require(before.decks[2]);
    REQUIRE(one.rate == 1.05);

    Relaunch launch{journaled(state.path(), kInterval)};
    const restart::State& state_now = launch.client.state;
    check_restored(state_now, 0, one, kBlock);
    check_restored(state_now, 2, three, kBlock);
    check_empty(state_now, 1);
    check_empty(state_now, 3);
    CHECK(eventually([&] {
        const auto mixer = state_now.mixer();
        return mixer && mixer_of(*mixer) == before.mixer;
    }));
    CHECK(eventually([&] {
        return has(contents(launch.session.engine_log()).value_or(""), "resumed from generation 1");
    }));

    // Each deck plays when asked, and is heard.
    std::expected<restart::Recording, std::string> recording =
        restart::Recording::open(launch.client);
    REQUIRE(recording.has_value());
    auto ring = restart::open_ring(launch.client);
    const std::uint64_t from = recording->reader().frames_written();
    REQUIRE(ring.ring.push(ks::track_loading::test::set_playing(0, true, 1)));
    REQUIRE(ring.ring.push(ks::track_loading::test::set_playing(2, true, 2)));
    CHECK(playing_deck(state_now, 0, one.position + (0.2 * restart::kRate)).has_value());
    CHECK(playing_deck(state_now, 2, three.position + (0.2 * restart::kRate)).has_value());
    CHECK(sounds(*recording, from));
}

TEST_CASE("Journal restore: a missing file is carried", "[journal][restore]") {
    const Files files;
    const Folder state;
    const fs::path first = restart::write_sine(files.path(), "first.wav", 60);
    const fs::path second = restart::write_constant(files.path(), "second.wav", 60);
    Journal journal;
    journal.decks[0] = entry_for(first, 2.0 * restart::kRate, 1.0 * restart::kRate, 1.0, true);
    journal.decks[2] = entry_for(second, 5.0 * restart::kRate);
    put_journal(state.path(), journal);
    const fs::path away = files.path() / "away.wav";
    fs::rename(first, away);

    {
        Relaunch launch{journaled(state.path(), kInterval)};
        check_empty(launch.client.state, 0);
        check_restored(launch.client.state, 2, restart::require(journal.decks[2]));
        CAPTURE(contents(launch.session.kool_spin_log()).value_or("(no log)"));
        CHECK(warned(launch.session, {"deck 1", first.string()}));
        CHECK(has(launch.session.stderr_text(), first.string()));
        CAPTURE(contents(journal_path(state.path())).value_or("(none)"));
        const Journal kept = restart::require(journal_from(state.path(), launch.session));
        CHECK(kept.decks[0] == carried(restart::require(journal.decks[0])));
        CHECK(restart::require(kept.decks[2]).path == second.string());
        stop_cleanly(launch.session);
    }

    // With the file back, both are restored.
    fs::rename(away, first);
    const Relaunch launch{journaled(state.path(), kInterval)};
    check_restored(launch.client.state, 0, carried(restart::require(journal.decks[0])));
    check_restored(launch.client.state, 2, restart::require(journal.decks[2]));
}

TEST_CASE("Journal restore: carried until the deck is used", "[journal][restore]") {
    const Files files;
    const Folder state;
    const fs::path first = restart::write_constant(files.path(), "first.wav", 60);
    const JournalEntry entry = entry_for(first, 3.0 * restart::kRate);
    Journal journal;
    journal.decks[0] = entry;
    put_journal(state.path(), journal);
    fs::remove(first);

    // Carried across three launches.
    for (int i = 0; i < 3; ++i) {
        CAPTURE(i);
        Relaunch launch{journaled(state.path(), kInterval)};
        check_empty(launch.client.state, 0);
        const Journal kept = restart::require(journal_from(state.path(), launch.session));
        CHECK(kept.decks[0] == entry);
        stop_cleanly(launch.session);
        CHECK(restart::require(read_journal(state.path())).decks[0] == entry);
    }

    // Loading another file on the deck replaces it.
    const fs::path other = restart::write_constant(files.path(), "other.wav", 30);
    {
        Relaunch launch{journaled(state.path(), kInterval)};
        REQUIRE(restart::is_loaded(restart::load(launch.client, 0, other)));
        CHECK(journal_from(state.path(), launch.session, [&](const Journal& j) {
                  return j.decks[0] && j.decks[0]->path == other.string();
              }).has_value());
        stop_cleanly(launch.session);
    }

    // On another launch, with that file gone too, unloading the deck removes
    // its entry.
    fs::remove(other);
    Relaunch launch{journaled(state.path(), kInterval)};
    check_empty(launch.client.state, 0);
    REQUIRE(journal_from(state.path(), launch.session, [&](const Journal& j) {
                return j.decks[0] && j.decks[0]->path == other.string();
            }).has_value());
    const std::optional<restart::Received> unloaded =
        restart::ask(launch.client, ks::ipc::msg::UnloadDeck{.deck = 0});
    CHECK((unloaded && std::holds_alternative<ks::ipc::msg::DeckUnloaded>(unloaded->message.body)));
    CHECK(journal_from(state.path(), launch.session, [](const Journal& j) {
              return !j.decks[0];
          }).has_value());
}

TEST_CASE("Journal restore: a changed file starts at 0", "[journal][restore]") {
    const Files files;
    const Folder state;
    constexpr std::uint64_t kFrames = std::uint64_t{30} * restart::kRate;
    const fs::path touched = write_wav_at(files.path(), "touched.wav", kFrames, restart::kRate);
    const fs::path resized = write_wav_at(files.path(), "resized.wav", kFrames, restart::kRate);
    const fs::path resampled = write_wav_at(files.path(), "resampled.wav", kFrames, restart::kRate);
    Journal journal;
    journal.decks[0] = entry_for(touched, 3.0 * restart::kRate, 1.0 * restart::kRate);
    journal.decks[1] = entry_for(resized, 3.0 * restart::kRate, 1.0 * restart::kRate);
    journal.decks[2] = entry_for(resampled, 3.0 * restart::kRate, 1.0 * restart::kRate);
    put_journal(state.path(), journal);

    // Its modification time changes, and nothing else.
    fs::last_write_time(touched, fs::last_write_time(touched) + std::chrono::seconds{10});
    // Its size changes, and nothing else.
    const fs::file_time_type resized_at = fs::last_write_time(resized);
    static_cast<void>(write_wav_at(files.path(), "resized.wav", kFrames + 1'000, restart::kRate));
    fs::last_write_time(resized, resized_at);
    // Another rate, with the same size and modification time.
    const fs::file_time_type modified = fs::last_write_time(resampled);
    static_cast<void>(write_wav_at(files.path(), "resampled.wav", kFrames, 48'000));
    fs::last_write_time(resampled, modified);
    REQUIRE(restart::require(status_of(resampled)).size == restart::require(journal.decks[2]).size);
    REQUIRE(restart::require(status_of(resampled)).modified ==
            restart::require(journal.decks[2]).modified);
    REQUIRE(restart::require(status_of(touched)).size == restart::require(journal.decks[0]).size);
    REQUIRE(restart::require(status_of(resized)).modified ==
            restart::require(journal.decks[1]).modified);

    const Relaunch launch{journaled(state.path(), kInterval)};
    for (std::size_t deck = 0; deck < 3; ++deck) {
        JournalEntry at_start = restart::require(std::span{journal.decks}[deck]);
        at_start.position = 0.0;
        at_start.cue = 0.0;
        if (deck == 2) {
            at_start.sample_rate = 48'000;
        }
        check_restored(launch.client.state, deck, at_start);
        CHECK(warned(launch.session, {std::format("deck {}", deck + 1), at_start.path}));
    }
    check_empty(launch.client.state, 3);
}

TEST_CASE("Journal restore: what can't be restored", "[journal][restore]") {
    const Files files;
    const Folder state;
    const fs::path text = files.path() / "notes.txt";
    write_text(text, "not a track\n");
    const fs::path slow = ks::track_loading::test::write_fake(
        files.path(), "slow.fake", FakeTrack{.estimate = 44'100, .probe_delay_ms = 5'000});
    const fs::path large = ks::track_loading::test::write_fake(
        files.path(), "large.fake", FakeTrack{.estimate = 1'000'000, .frames = 44'100});
    // It starts decoding only once the engine has started with it: after the
    // slow probe's 3 s.
    const fs::path fits = ks::track_loading::test::write_fake(
        files.path(), "fits.fake",
        FakeTrack{.estimate = 44'100, .decode_delay_ms = 6'000, .frames = 44'100});
    Journal journal;
    journal.decks[0] = entry_for(text, 1'000.0);
    journal.decks[1] = entry_for(slow, 1'000.0);
    journal.decks[2] = entry_for(large, 1'000.0);
    journal.decks[3] = entry_for(fits, 1'000.0);
    put_journal(state.path(), journal);

    restart::SessionOptions options = journaled(state.path(), kInterval, {"--track-memory", "1"});
    options.worker = std::string{ks::track_loading::test::kFakeWorkerPath};
    const auto launched = restart::Clock::now();
    const Relaunch launch{options};
    const auto took = std::chrono::duration_cast<Ms>(restart::Clock::now() - launched);
    CAPTURE(took.count());
    CHECK(took < (kSlowSanitizer ? Ms{6'000} : Ms{4'000}));
    CAPTURE(contents(launch.session.kool_spin_log()).value_or("(no log)"));

    check_empty(launch.client.state, 0);
    check_empty(launch.client.state, 1);
    check_empty(launch.client.state, 2);
    check_restored(launch.client.state, 3, restart::require(journal.decks[3]));
    CHECK(warned(launch.session, {"deck 1", text.string(), "not a fake track"}));
    CHECK(warned(launch.session, {"deck 2", slow.string(), "probed"}));
    CHECK(warned(launch.session, {"deck 3", large.string(), "track memory"}));
    // The slow probe's worker was stopped at the limit, not left to finish.
    const pid_t slow_worker = worker_pid(launch.session, 2);
    CHECK(eventually([&] { return ended(slow_worker); }, Ms{1'000}));

    const Journal kept = restart::require(journal_from(state.path(), launch.session));
    CHECK(kept.decks[0] == journal.decks[0]);
    CHECK(kept.decks[1] == journal.decks[1]);
    CHECK(kept.decks[2] == journal.decks[2]);
    CHECK(restart::require(kept.decks[3]).path == fits.string());
}

TEST_CASE("Journal restore: a journal that's refused", "[journal][restore]") {
    const Files files;
    const Folder state;
    const fs::path journal = journal_path(state.path());
    const fs::path rejected = state.path() / ks::supervisor::kRejectedJournalName;
    // An older one, which a refused journal replaces.
    write_text(rejected, "older\n");

    // A valid journal, with a deck whose file is there, made invalid.
    Journal valid;
    valid.decks[0] =
        entry_for(restart::write_constant(files.path(), "c.wav", 30), 1.0 * restart::kRate);
    const std::string text = ks::supervisor::format(valid);

    std::string bad;
    std::string why;
    bool kept_as_rejected = true;
    bool rewritten = true;
    std::optional<Mode> unreadable;
    SECTION("a bad checksum") {
        bad = text;
        const std::size_t digit =
            bad.rfind('\n', bad.size() - 2) + std::string_view{"checksum "}.size() + 1;
        bad[digit] = bad[digit] == '0' ? '1' : '0';
        why = "checksum";
    }
    SECTION("version 2") {
        std::string two = text;
        two.replace(0, std::string_view{"kool_spin session journal 1"}.size(),
                    "kool_spin session journal 2");
        bad = ks::supervisor::journal_test::with_checksum(two);
        why = "kool_spin session journal 1";
    }
    SECTION("128 KiB + 1 bytes") {
        bad = text;
        bad.resize(ks::supervisor::kMaxJournalSize + 1, '\n');
        why = "larger";
    }
    SECTION("a directory") {
        fs::create_directory(journal);
        why = "isn't a regular file";
        kept_as_rejected = false;
        rewritten = false;
    }
    SECTION("a file that can't be read") {
        skip_as_root();
        write_text(journal, text);
        unreadable.emplace(journal, 0000);
        why = "can't read";
        kept_as_rejected = false;
    }
    if (!bad.empty()) {
        write_text(journal, bad);
    }

    const Relaunch launch{journaled(state.path(), kInterval)};
    CAPTURE(contents(launch.session.kool_spin_log()).value_or("(no log)"));
    for (std::size_t deck = 0; deck < ks::supervisor::kJournalDecks; ++deck) {
        check_empty(launch.client.state, deck);
    }
    CHECK(warned(launch.session, {journal.string(), why}));
    CHECK(has(launch.session.stderr_text(), journal.string()));
    if (kept_as_rejected) {
        CHECK(contents(rejected) == bad);
    } else {
        CHECK(contents(rejected) == "older\n");
    }
    if (rewritten) {
        const Journal written = restart::require(journal_from(state.path(), launch.session));
        CHECK_FALSE(written.decks[0]);
    } else {
        CHECK(fs::is_directory(journal));
    }
}

TEST_CASE("Journal restore: --new-session", "[journal][restore]") {
    const Files files;
    const Folder state;
    Journal journal;
    journal.decks[0] =
        entry_for(restart::write_constant(files.path(), "c.wav", 30), 1.0 * restart::kRate);
    journal.mixer.crossfader = 0.2;
    put_journal(state.path(), journal);

    const Relaunch launch{journaled(state.path(), kInterval, {"--new-session"})};
    // Ready by now: the socket is open.
    const auto ready = restart::Clock::now();
    check_empty(launch.client.state, 0);
    CHECK(restart::real_of(restart::require(launch.client.state.mixer()).crossfader) == 0.5);
    const std::optional<Journal> written = journal_from(
        state.path(), launch.session,
        [](const Journal& j) {
            return std::ranges::none_of(j.decks, [](const auto& d) { return d.has_value(); }) &&
                   j.mixer == ks::supervisor::JournalMixer{};
        },
        2 * kInterval * kSlack + Ms{100});
    CAPTURE(std::chrono::duration_cast<Ms>(restart::Clock::now() - ready).count());
    CAPTURE(contents(journal_path(state.path())).value_or("(none)"));
    CHECK(written.has_value());
    CHECK(lines_with(launch.session, "info", "restoring") == 0);
}

TEST_CASE("Journal restore: after an orphan, decks move on", "[journal][restore]") {
    const Files files;
    const Folder state;
    const std::string boot = restart::require(machine_boot_id());
    const fs::path a = restart::write_sine(files.path(), "a.wav", 120);
    const fs::path b = restart::write_sine(files.path(), "b.wav", 120);
    const fs::path c = restart::write_sine(files.path(), "c.wav", 60);
    auto orphan = restart::start_playing(a, journaled(state.path(), kInterval));
    Playing& p = *orphan;
    REQUIRE(restart::is_loaded(restart::load(p.client, 1, b)));
    REQUIRE(restart::is_loaded(restart::load(p.client, 2, c)));
    p.send(CommandType::SetRate, 0, 0.9);
    p.send(CommandType::Seek, 1, 2.0 * restart::kRate);
    p.send(CommandType::SetRate, 1, -1.0);
    p.send(CommandType::Seek, 2, 5.0 * restart::kRate);
    REQUIRE(applied(p));
    REQUIRE(decoded(p.client.state, 0));
    REQUIRE(decoded(p.client.state, 1));
    REQUIRE(decoded(p.client.state, 2));
    // Deck 2 plays back to its start in 2 s, and stops there.
    p.play(0);
    p.play(1);
    REQUIRE(applied(p));
    const pid_t engine = p.session.engine();
    REQUIRE(journal_from(state.path(), p.session, [&](const Journal& j) {
                return j.decks[0] && j.decks[0]->playing && j.decks[1] && j.decks[1]->playing &&
                       j.decks[1]->rate == -1.0 && j.decks[2] &&
                       j.decks[2]->position == 5.0 * restart::kRate && j.boot == boot;
            }).has_value());
    p.session.process().signal(SIGKILL);
    REQUIRE(p.session.process().wait() == 128 + SIGKILL);
    const Journal journal = restart::require(read_journal(state.path()));
    CAPTURE(ks::supervisor::format(journal));
    REQUIRE(journal.engine == engine);
    std::this_thread::sleep_for(Ms{2'000});
    REQUIRE(!ended(engine));

    SECTION("the orphan's journal") {
        const Relaunch launch{journaled(state.path(), kInterval, {"--stop-previous"})};
        CHECK(ended(engine));
        // Where the orphan's decks were as it stopped.
        const ks::ipc::DeckState last0 = restart::require(p.client.state.deck(0));
        const ks::ipc::DeckState last1 = restart::require(p.client.state.deck(1));
        CAPTURE(restart::real_of(last0.position), restart::real_of(last1.position));

        // Each within 100 ms of playing of where the orphan had it.
        JournalEntry one = carried(restart::require(journal.decks[0]));
        one.position = restart::real_of(last0.position);
        check_restored(launch.client.state, 0, one, 0.1 * 0.9 * restart::kRate);
        CHECK(one.position >
              restart::require(journal.decks[0]).position + (2.0 * 0.9 * restart::kRate));
        JournalEntry two = carried(restart::require(journal.decks[1]));
        two.position = restart::real_of(last1.position);
        check_restored(launch.client.state, 1, two, 0.1 * restart::kRate);
        CHECK(restart::real_of(restart::require(launch.client.state.deck(1)).position) >= 0.0);
        check_restored(launch.client.state, 2, restart::require(journal.decks[2]));
    }
    SECTION("another engine's journal") {
        Journal other = journal;
        other.engine = engine == 1 ? 2 : engine - 1;
        put_journal(state.path(), other);
        const Relaunch launch{journaled(state.path(), kInterval, {"--stop-previous"})};
        CHECK(ended(engine));
        for (std::size_t deck = 0; deck < 3; ++deck) {
            check_restored(launch.client.state, deck,
                           carried(restart::require(std::span{journal.decks}[deck])));
        }
    }
    SECTION("another machine's journal") {
        Journal other = journal;
        other.boot = boot == "00000000-0000-0000-0000-000000000000"
                         ? "11111111-1111-1111-1111-111111111111"
                         : "00000000-0000-0000-0000-000000000000";
        put_journal(state.path(), other);
        const Relaunch launch{journaled(state.path(), kInterval, {"--stop-previous"})};
        CHECK(ended(engine));
        for (std::size_t deck = 0; deck < 3; ++deck) {
            check_restored(launch.client.state, deck,
                           carried(restart::require(std::span{journal.decks}[deck])));
        }
    }
}

TEST_CASE("Journal restore: an engine that fails starts clean", "[journal][restore]") {
    const Files files;
    const Folder state;
    Journal journal;
    journal.decks[0] =
        entry_for(restart::write_constant(files.path(), "one.wav", 30), 1.0 * restart::kRate);
    journal.decks[2] =
        entry_for(restart::write_constant(files.path(), "three.wav", 30), 2.0 * restart::kRate);
    journal.mixer.crossfader = 0.2;
    put_journal(state.path(), journal);

    // An engine that fails when it's given a track.
    const fs::path script = files.path() / "engine.sh";
    write_text(script, std::format("#!/bin/sh\nfor arg in \"$@\"; do\n"
                                   "  if [ \"$arg\" = --track ]; then exit 1; fi\n"
                                   "done\nexec '{}' \"$@\"\n",
                                   ks::processes::test::kEnginePath));
    fs::permissions(script, fs::perms::owner_all);
    restart::SessionOptions options = journaled(state.path(), kInterval);
    options.engine = script.string();

    Relaunch launch{options};
    CAPTURE(contents(launch.session.kool_spin_log()).value_or("(no log)"));
    CHECK(warned(launch.session, {"clean"}));
    for (std::size_t deck = 0; deck < ks::supervisor::kJournalDecks; ++deck) {
        check_empty(launch.client.state, deck);
    }
    CHECK(restart::real_of(restart::require(launch.client.state.mixer()).crossfader) == 0.5);
    CHECK(keeps_beating(launch.client.state));
    CHECK(launch.session.process().running());
    const Journal kept = restart::require(journal_from(state.path(), launch.session));
    CHECK(kept.decks[0] == journal.decks[0]);
    CHECK(kept.decks[2] == journal.decks[2]);
}

TEST_CASE("Journal restore: stopping while restoring", "[journal][restore]") {
    const Files files;
    const Folder state;
    Journal journal;
    journal.decks[0] = entry_for(
        ks::track_loading::test::write_fake(files.path(), "slow.fake",
                                            FakeTrack{.estimate = 44'100, .probe_delay_ms = 5'000}),
        1'000.0);
    put_journal(state.path(), journal);
    const std::optional<std::string> before = contents(journal_path(state.path()));

    restart::SessionOptions options = journaled(state.path(), kInterval);
    options.worker = std::string{ks::track_loading::test::kFakeWorkerPath};
    options.wait_for_socket = false;
    restart::Session session{options};
    REQUIRE(eventually([&] { return !worker_pids(session).empty(); }));
    const std::vector<pid_t> workers = worker_pids(session);

    const auto stopping = restart::Clock::now();
    session.process().signal(SIGTERM);
    const std::optional<int> outcome = session.process().wait(Ms{5'000});
    const auto took = std::chrono::duration_cast<Ms>(restart::Clock::now() - stopping);
    CAPTURE(took.count(), session.stderr_text());
    CAPTURE(contents(session.kool_spin_log()).value_or("(no log)"));
    CHECK(outcome == 0);
    CHECK(took < (kSlowSanitizer ? Ms{3'000} : Ms{1'500}));
    for (const pid_t worker : workers) {
        CHECK(ended(worker));
    }
    CHECK(lines_with(session, "info", "started ks-engine") == 0);
    CHECK_FALSE(lock_holder(session.dir(), kEngineLockName).has_value());
    CHECK_FALSE(lock_holder(state.path(), kEngineLockName).has_value());
    CHECK(contents(journal_path(state.path())) == before);
}

TEST_CASE("Journal restore: without the lock", "[journal][restore]") {
    skip_as_root();
    const Files files;
    const Folder state;
    Journal journal;
    journal.decks[0] =
        entry_for(restart::write_constant(files.path(), "c.wav", 30), 1.5 * restart::kRate);
    put_journal(state.path(), journal);
    const std::optional<std::string> before = contents(journal_path(state.path()));
    const fs::path lock = lock_path(state.path(), ks::ipc::kSupervisorLockName);
    write_text(lock, "");
    const Mode read_only{lock, 0400};

    const Relaunch launch{journaled(state.path(), kInterval)};
    CAPTURE(launch.session.stderr_text());
    CHECK(has(launch.session.stderr_text(), "won't be saved"));
    check_restored(launch.client.state, 0, restart::require(journal.decks[0]));
    std::this_thread::sleep_for(10 * kInterval);
    CHECK(contents(journal_path(state.path())) == before);
}

TEST_CASE("Journal restore: options", "[journal][options]") {
    const Files files;
    const Folder dir;
    const Finished twice =
        finished(files.path(), KS_KOOL_SPIN_PATH,
                 {"--runtime-dir", dir.path().string(), "--new-session", "--new-session"});
    CAPTURE(twice.out);
    CHECK(twice.status == 2);
    CHECK(has(twice.out, "kool_spin: "));
    const Finished help =
        finished(files.path(), KS_KOOL_SPIN_PATH, {"--runtime-dir", dir.path().string(), "--help"});
    CHECK(help.status == 0);
    CAPTURE(help.out);
    CHECK(has(help.out, "--new-session"));
}
