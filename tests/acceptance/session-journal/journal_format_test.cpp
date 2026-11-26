// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/session-journal/spec.md requirements 1 to 7: the
// shared limits, the journal's text, parsing it, the resume slot it restores,
// and the fuzz properties.

#include <ks/engine/block.h>
#include <ks/engine/deck.h>
#include <ks/engine/mixer.h>
#include <ks/engine/resume.h>
#include <ks/ipc/state.h>
#include <ks/ipc/track_segment.h>
#include <ks/supervisor/journal.h>

#include "journal_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace jt = ks::supervisor::journal_test;

// These tests write CHECK(!x) rather than CHECK_FALSE(x): in loops and
// helpers, clang-analyzer takes the flags CHECK_FALSE combines for an enum
// value out of range (optin.core.EnumCastOutOfRange).
namespace rt = ks::engine::resume_test;
using ks::supervisor::Journal;
using ks::supervisor::JournalEntry;
using ks::supervisor::JournalSide;
using ks::supervisor::kJournalDecks;
using ks::supervisor::kMaxJournalSize;

constexpr double kInf = std::numeric_limits<double>::infinity();

/// The journal of "a known journal": decks 1 and 3, a −∞ trim, no boot ID, a
/// negative modification time, and a path with every kind of character.
Journal known_journal() {
    Journal journal;
    journal.boot = std::nullopt;
    journal.engine = 4242;
    journal.time = 987'654'321'000;
    journal.decks[0] = JournalEntry{.path = "/music/My Mix \xc3\xa9\\a\tb\nc.flac",
                                    .size = 31'415'926,
                                    .modified = -1'500'000'000,
                                    .sample_rate = 44'100,
                                    .position = 551'250.5,
                                    .rate = 1.05,
                                    .cue = 44'100.0,
                                    .playing = true};
    journal.decks[2] = JournalEntry{.path = "/m/b.wav",
                                    .size = 0,
                                    .modified = 1'732'500'000'123'456'789,
                                    .sample_rate = 48'000,
                                    .position = 0.0,
                                    .rate = -1.5,
                                    .cue = 1'234.25,
                                    .playing = false};
    auto& channels = journal.mixer.channels;
    channels[0] = {.trim_db = -kInf, .fader = 1.0, .side = JournalSide::A, .cue = true};
    channels[1] = {.trim_db = 12.0, .fader = 0.75, .side = JournalSide::B, .cue = false};
    channels[2] = {.trim_db = -3.5, .fader = 0.0, .side = JournalSide::Thru, .cue = false};
    channels[3] = {};
    journal.mixer.crossfader = 0.25;
    journal.mixer.crossfader_transition = 0.01;
    journal.mixer.main_level_db = -6.0;
    journal.mixer.headphone_level_db = -kInf;
    journal.mixer.cue_mix = 1.0;
    journal.mixer.bus_main = 2;
    journal.mixer.bus_cue = 0;
    return journal;
}

/// Its text, from requirement 3.
std::string known_text() {
    const std::string body = "kool_spin session journal 1\n"
                             "boot none\n"
                             "engine 4242\n"
                             "time 987654321000\n"
                             "deck 1 path /music/My Mix \xc3\xa9\\\\a\\x09b\\x0ac.flac\n"
                             "deck 1 size 31415926\n"
                             "deck 1 modified -1500000000\n"
                             "deck 1 sample-rate 44100\n"
                             "deck 1 position 551250.5\n"
                             "deck 1 rate 1.05\n"
                             "deck 1 cue 44100\n"
                             "deck 1 playing 1\n"
                             "deck 3 path /m/b.wav\n"
                             "deck 3 size 0\n"
                             "deck 3 modified 1732500000123456789\n"
                             "deck 3 sample-rate 48000\n"
                             "deck 3 position 0\n"
                             "deck 3 rate -1.5\n"
                             "deck 3 cue 1234.25\n"
                             "deck 3 playing 0\n"
                             "channel 1 trim-db -inf\n"
                             "channel 1 fader 1\n"
                             "channel 1 side a\n"
                             "channel 1 cue 1\n"
                             "channel 2 trim-db 12\n"
                             "channel 2 fader 0.75\n"
                             "channel 2 side b\n"
                             "channel 2 cue 0\n"
                             "channel 3 trim-db -3.5\n"
                             "channel 3 fader 0\n"
                             "channel 3 side thru\n"
                             "channel 3 cue 0\n"
                             "channel 4 trim-db 0\n"
                             "channel 4 fader 1\n"
                             "channel 4 side thru\n"
                             "channel 4 cue 0\n"
                             "crossfader 0.25\n"
                             "crossfader-transition 0.01\n"
                             "main-level-db -6\n"
                             "headphone-level-db -inf\n"
                             "cue-mix 1\n"
                             "bus-main 2\n"
                             "bus-cue 0\n";
    return body + jt::checksum_line(body);
}

/// The value in `value`, which a test requires to be there. (clang-tidy
/// can't see through REQUIRE, so tests take the value this way.)
template <typename T>
T required(const std::optional<T>& value) {
    REQUIRE(value.has_value());
    return value.value_or(T{});
}

/// Requires that parse() refuses `text`, and says why.
void refused(const std::string& text, std::string_view what) {
    CAPTURE(what, text.size() < 400 ? text : text.substr(0, 400));
    REQUIRE(!text.empty());
    const std::expected<Journal, std::string> parsed = jt::parse_text(text);
    REQUIRE(!parsed.has_value());
    CHECK(!parsed.error().empty());
}

/// `text` with `key`'s value replaced, which must be refused.
void refused_value(const std::string& text, std::string_view key, std::string_view value) {
    const std::string changed = jt::with_value(text, key, value);
    CAPTURE(key, value);
    REQUIRE(!changed.empty());
    refused(changed, "a value");
}

double above(double value) {
    return std::nextafter(value, kInf);
}
double below(double value) {
    return std::nextafter(value, -kInf);
}

/// A snapshot with one empty deck at rate `rate`, for the limits test.
ks::engine::Snapshot snapshot_with(const std::function<void(ks::engine::Snapshot&)>& change) {
    ks::engine::Snapshot snapshot;
    snapshot.generation = 1;
    snapshot.host_time = rt::at_ns(0);
    change(snapshot);
    return snapshot;
}

bool validates(const ks::engine::Snapshot& snapshot) {
    return ks::engine::validate(rt::expected_words(snapshot), 1).has_value();
}

} // namespace

TEST_CASE("Journal format: shared limits", "[journal][format]") {
    STATIC_REQUIRE(ks::engine::kMaxRate == ks::ipc::kMaxDeckRate);
    STATIC_REQUIRE(ks::ipc::kMaxDeckRate == 4.0);
    STATIC_REQUIRE(ks::engine::kMaxTrimDb == ks::ipc::kMaxTrimDb);
    STATIC_REQUIRE(ks::ipc::kMaxTrimDb == 12.0);
    STATIC_REQUIRE(ks::engine::kMinCrossfaderTransition == ks::ipc::kMinCrossfaderTransition);
    STATIC_REQUIRE(ks::ipc::kMinCrossfaderTransition == 0.01);
    STATIC_REQUIRE(ks::engine::kMaxOutputChannels == ks::ipc::kMaxOutputChannels);
    STATIC_REQUIRE(ks::ipc::kMaxOutputChannels == 8);

    using ks::engine::Snapshot;
    // Each limit, and one step past it.
    for (const double rate : {ks::ipc::kMaxDeckRate, -ks::ipc::kMaxDeckRate}) {
        CHECK(validates(snapshot_with([&](Snapshot& s) { s.decks[0].rate = rate; })));
        CHECK(!validates(snapshot_with(
            [&](Snapshot& s) { s.decks[0].rate = rate > 0 ? above(rate) : below(rate); })));
    }
    CHECK(validates(
        snapshot_with([](Snapshot& s) { s.mixer.channels[0].trim_db = ks::ipc::kMaxTrimDb; })));
    CHECK(!validates(snapshot_with(
        [](Snapshot& s) { s.mixer.channels[0].trim_db = above(ks::ipc::kMaxTrimDb); })));
    CHECK(validates(snapshot_with(
        [](Snapshot& s) { s.mixer.crossfader_transition = ks::ipc::kMinCrossfaderTransition; })));
    CHECK(!validates(snapshot_with([](Snapshot& s) {
        s.mixer.crossfader_transition = below(ks::ipc::kMinCrossfaderTransition);
    })));
    CHECK(validates(snapshot_with([](Snapshot& s) {
        s.mixer.bus_map = {.main = ks::ipc::kMaxOutputChannels - 2, .cue = 0};
    })));
    CHECK(!validates(snapshot_with(
        [](Snapshot& s) { s.mixer.bus_map = {.main = ks::ipc::kMaxOutputChannels, .cue = 0}; })));
}

TEST_CASE("Journal format: a known journal", "[journal][format]") {
    const Journal journal = known_journal();
    REQUIRE(jt::values_allowed(journal));
    const std::string text = ks::supervisor::format(journal);
    CHECK(text == known_text());

    const std::expected<Journal, std::string> parsed = jt::parse_text(known_text());
    REQUIRE(parsed.has_value());
    CHECK(*parsed == journal);
    CHECK(std::signbit(parsed->mixer.channels[0].trim_db));
    CHECK(std::isinf(parsed->mixer.channels[0].trim_db));

    SECTION("an empty session has no deck lines") {
        Journal empty;
        empty.boot = std::string{"0123abcd-ef"};
        const std::string empty_text = ks::supervisor::format(empty);
        CHECK(empty_text.find("\ndeck ") == std::string::npos);
        CHECK(empty_text.starts_with("kool_spin session journal 1\nboot 0123abcd-ef\nengine 0\n"
                                     "time 0\nchannel 1 trim-db 0\n"));
        const std::expected<Journal, std::string> back = jt::parse_text(empty_text);
        REQUIRE(back.has_value());
        CHECK(*back == empty);
    }
}

TEST_CASE("Journal format: refusals", "[journal][format]") {
    const std::string text = known_text();
    REQUIRE(jt::parse_text(text).has_value());
    const std::vector<std::string> lines = jt::lines_of(text);
    const std::size_t body = lines.size() - 1; // the last is the checksum

    SECTION("a line missing, doubled, or swapped with the next") {
        for (std::size_t i = 0; i < body; ++i) {
            CAPTURE(lines[i]);
            std::vector<std::string> missing = lines;
            missing.erase(missing.begin() + static_cast<std::ptrdiff_t>(i));
            refused(jt::with_checksum(jt::joined(missing)), "missing");

            std::vector<std::string> doubled = lines;
            doubled.insert(doubled.begin() + static_cast<std::ptrdiff_t>(i), lines[i]);
            refused(jt::with_checksum(jt::joined(doubled)), "doubled");

            if (i + 1 < body) {
                std::vector<std::string> swapped = lines;
                std::swap(swapped[i], swapped[i + 1]);
                refused(jt::with_checksum(jt::joined(swapped)), "swapped");
            }
        }
        std::vector<std::string> no_checksum = lines;
        no_checksum.pop_back();
        refused(jt::joined(no_checksum), "no checksum line");
        std::vector<std::string> two = lines;
        two.push_back(lines.back());
        refused(jt::joined(two), "two checksum lines");
    }

    SECTION("a wrong checksum") {
        std::string digit = text;
        const std::size_t at = digit.size() - 2; // the last hex digit
        digit[at] = digit[at] == '0' ? '1' : '0';
        refused(digit, "a digit");

        // A journal whose checksum has a letter in it, in upper case.
        std::string lettered;
        for (std::int32_t pid = 1; pid < 1'000; ++pid) {
            lettered = jt::with_value(text, "engine", std::to_string(pid));
            const std::string_view crc = std::string_view{lettered}.substr(lettered.size() - 9, 8);
            if (crc.find_first_of("abcdef") != std::string_view::npos) {
                break;
            }
        }
        REQUIRE(jt::parse_text(lettered).has_value());
        std::string upper = lettered;
        for (std::size_t i = upper.size() - 9; i < upper.size() - 1; ++i) {
            if (upper[i] >= 'a' && upper[i] <= 'f') {
                upper[i] = static_cast<char>(upper[i] - 'a' + 'A');
            }
        }
        REQUIRE(upper != lettered);
        refused(upper, "upper case");

        refused(text.substr(0, text.size() - 2) + "\n", "7 digits");
        std::string changed = text;
        changed.replace(changed.find("engine 4242"), 11, "engine 4243");
        refused(changed, "a body the checksum isn't of");
    }

    SECTION("another version") {
        std::string two = text;
        two.replace(0, std::string_view{"kool_spin session journal 1"}.size(),
                    "kool_spin session journal 2");
        refused(jt::with_checksum(two), "version 2");
    }

    SECTION("bytes format() wouldn't write around the lines") {
        refused(text + "x", "a trailing byte");
        refused(text + "\n", "a trailing newline");
        refused(text.substr(0, text.size() - 1), "no final newline");
        std::string crlf;
        for (const std::string& line : jt::lines_of(text)) {
            crlf += line + "\r\n";
        }
        refused(jt::with_checksum(crlf), "CRLF");
        refused(jt::with_value(text, "engine", "4242 "), "a trailing space");
        std::string spaced = text;
        spaced.replace(spaced.find("engine 4242"), 11, "engine  4242");
        refused(jt::with_checksum(spaced), "two spaces");
        std::string tabbed = text;
        tabbed.replace(tabbed.find("engine 4242"), 11, "engine\t4242");
        refused(jt::with_checksum(tabbed), "a tab");
    }

    SECTION("integers and reals not as format() writes them") {
        refused_value(text, "engine", "04242");
        refused_value(text, "engine", "+4242");
        refused_value(text, "deck 3 size", "00");
        refused_value(text, "deck 3 modified", "-0");
        refused_value(text, "deck 1 sample-rate", "44100.0");
        refused_value(text, "channel 1 fader", "1.0");
        refused_value(text, "deck 1 position", "551250.50");
        refused_value(text, "crossfader", ".25");
        refused_value(text, "crossfader", "2.5e-1");
        refused_value(text, "deck 1 rate", "1.05e0");
        refused_value(text, "cue-mix", "+1");
        refused_value(text, "deck 3 position", "0x0");
        refused_value(text, "engine", "");
    }

    SECTION("NaN, and infinities where they aren't allowed") {
        refused_value(text, "deck 1 rate", "nan");
        refused_value(text, "deck 1 rate", "-nan");
        refused_value(text, "channel 2 trim-db", "nan");
        refused_value(text, "channel 2 trim-db", "inf");
        refused_value(text, "main-level-db", "inf");
        refused_value(text, "crossfader", "inf");
        refused_value(text, "crossfader", "-inf");
        refused_value(text, "channel 1 fader", "-inf");
        refused_value(text, "deck 1 position", "-inf");
        refused_value(text, "deck 1 position", "inf");
        refused_value(text, "deck 1 cue", "-inf");
        refused_value(text, "deck 1 rate", "-inf");
        refused_value(text, "cue-mix", "-inf");
        refused_value(text, "crossfader-transition", "-inf");
        refused_value(text, "channel 1 trim-db", "-Infinity");
    }

    SECTION("one step past each bound") {
        const double max_frames = jt::kMaxFrames;
        for (const std::string_view key : {"deck 1 position", "deck 1 cue"}) {
            refused_value(text, key, jt::real(below(0.0)));
            refused_value(text, key, jt::real(above(max_frames)));
        }
        refused_value(text, "deck 1 rate", jt::real(above(ks::ipc::kMaxDeckRate)));
        refused_value(text, "deck 1 rate", jt::real(below(-ks::ipc::kMaxDeckRate)));
        refused_value(text, "deck 1 sample-rate", std::to_string(ks::ipc::kMinTrackRate - 1));
        refused_value(text, "deck 1 sample-rate", std::to_string(ks::ipc::kMaxTrackRate + 1));
        refused_value(text, "deck 1 size", "9223372036854775808");
        refused_value(text, "deck 1 modified", "9223372036854775808");
        refused_value(text, "deck 1 modified", "-9223372036854775809");
        refused_value(text, "deck 1 playing", "2");
        refused_value(text, "engine", "2147483648");
        refused_value(text, "engine", "-1");
        refused_value(text, "time", "9223372036854775808");
        refused_value(text, "time", "-1");
        refused_value(text, "boot", std::string(ks::supervisor::kMaxBootIdSize + 1, 'a'));
        refused_value(text, "boot", "A");
        refused_value(text, "boot", "g");
        refused_value(text, "boot", "NONE");
        refused_value(text, "channel 2 trim-db", jt::real(above(ks::ipc::kMaxTrimDb)));
        refused_value(text, "channel 2 fader", jt::real(above(1.0)));
        refused_value(text, "channel 2 fader", jt::real(below(0.0)));
        refused_value(text, "channel 2 side", "c");
        refused_value(text, "channel 2 side", "A");
        refused_value(text, "channel 2 cue", "2");
        refused_value(text, "crossfader", jt::real(above(1.0)));
        refused_value(text, "crossfader", jt::real(below(0.0)));
        refused_value(text, "crossfader-transition",
                      jt::real(below(ks::ipc::kMinCrossfaderTransition)));
        refused_value(text, "crossfader-transition", jt::real(above(1.0)));
        refused_value(text, "main-level-db", jt::real(above(0.0)));
        refused_value(text, "headphone-level-db", jt::real(above(0.0)));
        refused_value(text, "cue-mix", jt::real(above(1.0)));
        refused_value(text, "cue-mix", jt::real(below(0.0)));
        refused_value(text, "bus-main", "1");
        refused_value(text, "bus-main", std::to_string(ks::ipc::kMaxOutputChannels));
        refused_value(text, "bus-main", "0"); // the same as bus-cue
        refused_value(text, "bus-cue", std::to_string(ks::ipc::kMaxOutputChannels));
        refused_value(text, "bus-cue", "3");
    }

    SECTION("paths") {
        const std::string_view key = "deck 3 path";
        refused_value(text, key, "/m/\\q.wav");
        refused_value(text, key, "/m/\\x4.wav");
        refused_value(text, key, "/m/\\x41.wav"); // 'A' needs no escape
        refused_value(text, key, "/m/\\x0A.wav"); // upper case
        refused_value(text, key, "/m/a\\");
        refused_value(text, key, "/m/a\tb.wav");
        refused_value(text, key, "/m/a\x7f.wav");
        refused_value(text, key, "m/b.wav");
        refused_value(text, key, "");
        refused_value(text, key, "/" + std::string(ks::supervisor::kMaxJournalPathSize, 'x'));
        refused_value(text, key, "/m/\xff.wav");
        refused_value(text, key, "/m/\xc0\xaf.wav");     // overlong
        refused_value(text, key, "/m/\xed\xa0\x80.wav"); // a surrogate
        refused_value(text, key, "/m/\xc3");             // cut short

        // The longest path, counted once unescaped, is accepted.
        const std::string longest =
            "/" + std::string(ks::supervisor::kMaxJournalPathSize - 2, 'x') + "\\x09";
        const std::expected<Journal, std::string> parsed =
            jt::parse_text(jt::with_value(text, key, longest));
        REQUIRE(parsed.has_value());
        CHECK(required(parsed->decks[2]).path.size() == ks::supervisor::kMaxJournalPathSize);
    }

    SECTION("deck numbers") {
        std::string zero = text;
        for (std::size_t at = zero.find("deck 1 "); at != std::string::npos;
             at = zero.find("deck 1 ", at)) {
            zero.replace(at, 7, "deck 0 ");
        }
        refused(jt::with_checksum(zero), "deck 0");
        std::string five = text;
        for (std::size_t at = five.find("deck 3 "); at != std::string::npos;
             at = five.find("deck 3 ", at)) {
            five.replace(at, 7, "deck 5 ");
        }
        refused(jt::with_checksum(five), "deck 5");

        // Deck 3's lines before deck 1's.
        std::vector<std::string> reordered(lines.begin(), lines.begin() + 4);
        reordered.insert(reordered.end(), lines.begin() + 12, lines.begin() + 20);
        reordered.insert(reordered.end(), lines.begin() + 4, lines.begin() + 12);
        reordered.insert(reordered.end(), lines.begin() + 20,
                         lines.begin() + static_cast<std::ptrdiff_t>(body));
        refused(jt::with_checksum(jt::joined(reordered)), "decks out of order");
    }

    SECTION("more than kMaxJournalSize bytes") {
        // No journal format() writes is that big, so anything that is would
        // be refused anyway: the reason shows that the size was checked, before
        // anything else.
        std::string big = text;
        big.resize(kMaxJournalSize + 1, '\n');
        refused(big, "too big");
        const std::expected<Journal, std::string> parsed = jt::parse_text(big);
        REQUIRE(!parsed.has_value());
        CAPTURE(parsed.error());
        CHECK(parsed.error().find(std::to_string(kMaxJournalSize)) != std::string::npos);
    }
}

TEST_CASE("Journal format: the resume slot", "[journal][format]") {
    const Journal journal = known_journal();
    const auto host_time = rt::at_ns(5'000'000'000);

    SECTION("the known journal") {
        // Deck 1 has an entry and an ID; deck 2 an ID and no entry; deck 3 an
        // entry and ID 0; deck 4 neither.
        const std::array<std::uint64_t, kJournalDecks> ids{7, 5, 0, 0};
        ks::engine::Snapshot expected;
        expected.generation = 3;
        expected.host_time = host_time;
        expected.decks[0] = {
            .track_id = 7, .playing = false, .position = 551'250.5, .rate = 1.05, .cue = 44'100.0};
        auto& mixer = expected.mixer;
        mixer.channels[0] = {
            .trim_db = -kInf, .fader = 1.0, .side = ks::engine::CrossfaderSide::A, .cue = true};
        mixer.channels[1] = {
            .trim_db = 12.0, .fader = 0.75, .side = ks::engine::CrossfaderSide::B, .cue = false};
        mixer.channels[2] = {
            .trim_db = -3.5, .fader = 0.0, .side = ks::engine::CrossfaderSide::Thru, .cue = false};
        mixer.crossfader = 0.25;
        mixer.crossfader_transition = 0.01;
        mixer.main_level_db = -6.0;
        mixer.headphone_level_db = -kInf;
        mixer.cue_mix = 1.0;
        mixer.bus_map = {.main = 2, .cue = 0};

        const ks::ipc::ResumeState words = ks::supervisor::resume_state(journal, ids, 3, host_time);
        const ks::ipc::ResumeState wanted = ks::engine::encode(expected);
        CAPTURE(rt::word_differences(words, wanted));
        CHECK(rt::same_words(words, wanted));
        CHECK(rt::same_snapshot(required(ks::engine::validate(words, 1)), expected));
        CHECK(!ks::engine::validate(words, 0).has_value());

        // Deck 3 with an ID: there, paused, in reverse.
        const std::array<std::uint64_t, kJournalDecks> all{7, 5, 9, 11};
        const ks::engine::Snapshot both = required(
            ks::engine::validate(ks::supervisor::resume_state(journal, all, 4, host_time), 0));
        CHECK(both.decks[0].track_id == 7);
        CHECK(both.decks[1] == ks::engine::DeckSnapshot{});
        CHECK(both.decks[2] ==
              ks::engine::DeckSnapshot{
                  .track_id = 9, .playing = false, .position = 0.0, .rate = -1.5, .cue = 1'234.25});
        CHECK(both.decks[3] == ks::engine::DeckSnapshot{});
        for (const ks::engine::DeckSnapshot& deck : both.decks) {
            CHECK(!deck.playing);
        }
    }

    SECTION("a default journal has the engine's default mixer") {
        const std::array<std::uint64_t, kJournalDecks> none{};
        const ks::engine::Snapshot read = required(
            ks::engine::validate(ks::supervisor::resume_state(Journal{}, none, 1, host_time), 1));
        CHECK(read.mixer == ks::engine::MixerSettings{});
        for (const ks::engine::DeckSnapshot& deck : read.decks) {
            CHECK(deck == ks::engine::DeckSnapshot{});
        }
    }

    SECTION("every bound of requirement 5") {
        using Change = std::function<void(Journal&)>;
        const double max_frames = jt::kMaxFrames;
        const std::vector<std::pair<std::string_view, Change>> bounds{
            {"position 0", [](Journal& j) { j.decks[0]->position = 0.0; }},
            {"position max", [&](Journal& j) { j.decks[0]->position = max_frames; }},
            {"cue max", [&](Journal& j) { j.decks[0]->cue = max_frames; }},
            {"rate max", [](Journal& j) { j.decks[0]->rate = ks::ipc::kMaxDeckRate; }},
            {"rate min", [](Journal& j) { j.decks[0]->rate = -ks::ipc::kMaxDeckRate; }},
            {"sample rate min",
             [](Journal& j) { j.decks[0]->sample_rate = ks::ipc::kMinTrackRate; }},
            {"sample rate max",
             [](Journal& j) { j.decks[0]->sample_rate = ks::ipc::kMaxTrackRate; }},
            {"size max", [](Journal& j) { j.decks[0]->size = jt::kMaxSize; }},
            {"modified min",
             [](Journal& j) { j.decks[0]->modified = std::numeric_limits<std::int64_t>::min(); }},
            {"engine max", [](Journal& j) { j.engine = jt::kMaxPid; }},
            {"time max", [](Journal& j) { j.time = std::numeric_limits<std::int64_t>::max(); }},
            {"boot longest",
             [](Journal& j) { j.boot = std::string(ks::supervisor::kMaxBootIdSize, 'f'); }},
            {"trim max", [](Journal& j) { j.mixer.channels[1].trim_db = ks::ipc::kMaxTrimDb; }},
            {"fader 0", [](Journal& j) { j.mixer.channels[1].fader = 0.0; }},
            {"crossfader 1", [](Journal& j) { j.mixer.crossfader = 1.0; }},
            {"crossfader 0", [](Journal& j) { j.mixer.crossfader = 0.0; }},
            {"transition 1", [](Journal& j) { j.mixer.crossfader_transition = 1.0; }},
            {"main level 0", [](Journal& j) { j.mixer.main_level_db = 0.0; }},
            {"main level -inf", [](Journal& j) { j.mixer.main_level_db = -kInf; }},
            {"headphone level 0", [](Journal& j) { j.mixer.headphone_level_db = 0.0; }},
            {"cue mix 0", [](Journal& j) { j.mixer.cue_mix = 0.0; }},
            {"buses at the top",
             [](Journal& j) {
                 j.mixer.bus_main = ks::ipc::kMaxOutputChannels - 2;
                 j.mixer.bus_cue = ks::ipc::kMaxOutputChannels - 4;
             }},
        };
        const std::array<std::uint64_t, kJournalDecks> ids{1, 2, 3, 4};
        for (const auto& [name, change] : bounds) {
            CAPTURE(name);
            Journal bound = known_journal();
            change(bound);
            REQUIRE(jt::values_allowed(bound));
            const std::string text = ks::supervisor::format(bound);
            const std::expected<Journal, std::string> parsed = jt::parse_text(text);
            REQUIRE(parsed.has_value());
            CHECK(*parsed == bound);
            CHECK(jt::slot_check(*parsed, ids, 1));
            CHECK(jt::slot_check(*parsed, ids, ks::engine::kMaxGeneration));
        }
    }
}

TEST_CASE("Journal format: fuzz checks", "[journal][format]") {
    // Requirement 7's checks, as the fuzz target runs them, on 100,000 inputs
    // from a fixed seed.
    std::mt19937_64 random{1};
    std::uniform_int_distribution<std::size_t> length{0, 1'200};
    std::uniform_int_distribution<unsigned> byte{0, 255};
    std::vector<std::uint8_t> input;
    input.reserve(1'200);
    std::size_t failures = 0;
    for (int i = 0; i < 100'000; ++i) {
        input.resize(length(random));
        for (std::uint8_t& b : input) {
            b = static_cast<std::uint8_t>(byte(random));
        }
        if (!jt::check_input(input)) {
            ++failures;
        }
    }
    REQUIRE(failures == 0);
}
