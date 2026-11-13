// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/harness/spec.md: reading scripts (requirements
// 4 to 16).

#include <ks/harness/script.h>

#include <ks/engine/command.h>
#include <ks/engine/mixer.h>
#include <ks/ipc/command.h>

#include "script_properties.h"
#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ks::engine::BusMap;
using ks::engine::Command;
using ks::engine::CrossfaderSide;
using ks::harness::command_for;
using ks::harness::Event;
using ks::harness::kMaxEvents;
using ks::harness::kMaxScriptBytes;
using ks::harness::kMaxTracks;
using ks::harness::Load;
using ks::harness::read_script;
using ks::harness::Script;
using ks::harness::ScriptError;
using ks::harness::Signal;
using ks::harness::SignalKind;
using ks::harness::TrackFile;
using ks::harness::TrackSpec;
using ks::harness::TrackTime;
using ks::harness::Unload;
using ks::harness::test::broken_property;
using ks::harness::test::error_text;
using ks::harness::test::lines;
using ks::ipc::CommandType;

using Read = std::expected<Script, ScriptError>;

const std::filesystem::path kDirectory{"/scripts"};
constexpr double kMinusInfinity = -std::numeric_limits<double>::infinity();

Event at(double time, std::size_t line, const ks::harness::Action& action) {
    return Event{.time = time, .action = action, .line = line};
}

Command value(CommandType type, std::size_t deck, double v) {
    return Command{.type = type, .deck = deck, .value = v};
}

Command global(CommandType type, double v) {
    return Command{.type = type, .value = v};
}

Command flag(CommandType type, std::size_t deck, bool on) {
    return Command{.type = type, .deck = deck, .on = on};
}

Command side(std::size_t deck, CrossfaderSide s) {
    return Command{.type = CommandType::SetCrossfaderAssign, .deck = deck, .side = s};
}

/// Reads `text`, which must be a valid script.
Script read_valid(const std::string& text) {
    const Read read = read_script(text, kDirectory);
    CAPTURE(error_text(read));
    REQUIRE(read.has_value());
    return *read;
}

// Every track source and every command. Line n of the script is
// kEveryLine[n - 1].
constexpr std::array<std::string_view, 34> kEveryLine{
    "# Every track source, and every command.",
    "track kick clicks 120 30",
    "track tone sine 1000 30 rate 48000",
    "track glide sweep 20 20000 30 rate 96000",
    "track hiss noise 30",
    R"(track song file "music/one track.flac"   # a space in the path)",
    "",
    "0 load 1 kick",
    "0 load 2 tone",
    "0 play 1",
    "0.5 rate 1 1.02",
    "1 seek 2 2.5",
    "1 cuepoint 2 0.25",
    "1.5 trim 3 -6",
    "1.5 trim 4 -inf",
    "2 fader 2 0.8",
    "2 assign 1 a",
    "2 assign 2 b",
    "2 assign 3 thru",
    "2 pfl 1 on",
    "2 pfl 1 off",
    "3 crossfader 0.25",
    "3 curve 0.05",
    "3 main -3",
    "3 headphones -12.5",
    "3 cuemix 0.5",
    "3 busmap 3 1",
    "4 pause 1",
    "4 rate 2 -1",
    "5 unload 1",
    "5 load 1 song",
    "2.5\tload 3 glide\t# a tab, and out of time order",
    "2.5 load 4 hiss",
    "6 end",
};

} // namespace

TEST_CASE("Script: tracks and timed commands", "[harness]") {
    const Script script = read_valid(lines(kEveryLine));

    SECTION("the tracks, in script order") {
        const std::vector<TrackSpec> expected{
            TrackSpec{.name = "kick",
                      .source = Signal{.kind = SignalKind::Clicks, .bpm = 120, .seconds = 30},
                      .line = 2},
            TrackSpec{.name = "tone",
                      .source = Signal{.kind = SignalKind::Sine,
                                       .frequency_hz = 1000,
                                       .seconds = 30,
                                       .sample_rate = 48'000},
                      .line = 3},
            TrackSpec{.name = "glide",
                      .source = Signal{.kind = SignalKind::Sweep,
                                       .frequency_hz = 20,
                                       .to_hz = 20'000,
                                       .seconds = 30,
                                       .sample_rate = 96'000},
                      .line = 4},
            TrackSpec{.name = "hiss",
                      .source = Signal{.kind = SignalKind::Noise, .seconds = 30},
                      .line = 5},
            TrackSpec{.name = "song",
                      .source = TrackFile{.path = kDirectory / "music/one track.flac"},
                      .line = 6},
        };
        REQUIRE(script.tracks.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CAPTURE(i);
            CHECK(script.tracks.at(i) == expected.at(i));
        }
    }

    SECTION("the events, 0-based, in run order") {
        const std::vector<Event> expected{
            at(0, 8, Load{.deck = 0, .track = 0}),
            at(0, 9, Load{.deck = 1, .track = 1}),
            at(0, 10, flag(CommandType::SetPlaying, 0, true)),
            at(0.5, 11, value(CommandType::SetRate, 0, 1.02)),
            at(1, 12, TrackTime{.type = CommandType::Seek, .deck = 1, .seconds = 2.5, .track = 1}),
            at(1, 13,
               TrackTime{.type = CommandType::SetCuePoint, .deck = 1, .seconds = 0.25, .track = 1}),
            at(1.5, 14, value(CommandType::SetTrim, 2, -6)),
            at(1.5, 15, value(CommandType::SetTrim, 3, kMinusInfinity)),
            at(2, 16, value(CommandType::SetFader, 1, 0.8)),
            at(2, 17, side(0, CrossfaderSide::A)),
            at(2, 18, side(1, CrossfaderSide::B)),
            at(2, 19, side(2, CrossfaderSide::Thru)),
            at(2, 20, flag(CommandType::SetChannelCue, 0, true)),
            at(2, 21, flag(CommandType::SetChannelCue, 0, false)),
            at(2.5, 32, Load{.deck = 2, .track = 2}),
            at(2.5, 33, Load{.deck = 3, .track = 3}),
            at(3, 22, global(CommandType::SetCrossfader, 0.25)),
            at(3, 23, global(CommandType::SetCrossfaderCurve, 0.05)),
            at(3, 24, global(CommandType::SetMainLevel, -3)),
            at(3, 25, global(CommandType::SetHeadphoneLevel, -12.5)),
            at(3, 26, global(CommandType::SetCueMix, 0.5)),
            at(3, 27,
               Command{.type = CommandType::SetBusMap, .bus_map = BusMap{.main = 2, .cue = 0}}),
            at(4, 28, flag(CommandType::SetPlaying, 0, false)),
            at(4, 29, value(CommandType::SetRate, 1, -1)),
            at(5, 30, Unload{.deck = 0}),
            at(5, 31, Load{.deck = 0, .track = 4}),
        };
        REQUIRE(script.events.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CAPTURE(i, script.events.at(i).line, expected.at(i).line);
            CHECK(script.events.at(i) == expected.at(i));
        }
        CHECK(script.end == 6.0);
    }

    SECTION("seek and cue point become frames at the track's rate") {
        const TrackTime seek{.type = CommandType::Seek, .deck = 1, .seconds = 2.5, .track = 1};
        CHECK(command_for(seek, 48'000) == value(CommandType::Seek, 1, 120'000));
        const TrackTime cue{.type = CommandType::SetCuePoint, .deck = 3, .seconds = 0.25};
        CHECK(command_for(cue, 44'100) == value(CommandType::SetCuePoint, 3, 11'025));
    }

    SECTION("it keeps every property the fuzz target checks") {
        const std::optional<std::string> broken = broken_property(script);
        CAPTURE(broken.value_or(""));
        CHECK(broken == std::nullopt);
    }

    SECTION("CRLF line ends, blank lines, comments, and # in a path") {
        const Script crlf = read_valid("track a file \"x#1.wav\"\r\n"
                                       "   \t  \r\n"
                                       "# only a comment\r\n"
                                       "track b file \"/music/b.wav\" # an absolute path\r\n"
                                       "0 load 1 a # after a command\r\n"
                                       "0 load 2 b\r\n"
                                       "1 end\r\n");
        REQUIRE(crlf.tracks.size() == 2);
        CHECK(crlf.tracks.at(0).source ==
              ks::harness::TrackSpec::Source{TrackFile{.path = kDirectory / "x#1.wav"}});
        CHECK(crlf.tracks.at(1).source ==
              ks::harness::TrackSpec::Source{TrackFile{.path = "/music/b.wav"}});
        REQUIRE(crlf.events.size() == 2);
        CHECK(crlf.events.at(0) == at(0, 5, Load{.deck = 0, .track = 0}));
        CHECK(crlf.end == 1.0);
    }
}

TEST_CASE("Script: repeat blocks", "[harness]") {
    SECTION("a block runs each line once per repetition, in run order with the rest") {
        constexpr std::array<std::string_view, 10> kLines{
            "track t sine 440 60", // 1
            "0 load 1 t",          // 2
            "10 repeat 3 every 2", // 3
            "0 play 1",            // 4
            "0.5 rate 1 1.5",      // 5
            "1.5 pause 1",         // 6
            "end repeat",          // 7
            "11 fader 1 0.5",      // 8
            "12 crossfader 1",     // 9: at the time of repetition 1's play
            "20 end",              // 10
        };
        const Script script = read_valid(lines(kLines));
        const Command play = flag(CommandType::SetPlaying, 0, true);
        const Command rate = value(CommandType::SetRate, 0, 1.5);
        const Command pause = flag(CommandType::SetPlaying, 0, false);
        const std::vector<Event> expected{
            at(0, 2, Load{.deck = 0, .track = 0}),
            at(10, 4, play),
            at(10.5, 5, rate),
            at(11, 8, value(CommandType::SetFader, 0, 0.5)),
            at(11.5, 6, pause),
            at(12, 4, play),
            at(12, 9, global(CommandType::SetCrossfader, 1)),
            at(12.5, 5, rate),
            at(13.5, 6, pause),
            at(14, 4, play),
            at(14.5, 5, rate),
            at(15.5, 6, pause),
        };
        REQUIRE(script.events.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CAPTURE(i);
            CHECK(script.events.at(i) == expected.at(i));
        }
    }

    SECTION("a block may expand to exactly the most events allowed") {
        const Script script = read_valid("0 repeat 100000 every 0.001\n"
                                         "0 crossfader 0.5\n"
                                         "end repeat\n"
                                         "200 end\n");
        REQUIRE(script.events.size() == kMaxEvents);
        CHECK(script.events.front().time == 0.0);
        const double last = 0.0 + (99'999.0 * 0.001) + 0.0;
        CHECK(std::abs(script.events.back().time - last) < 1e-9);
        CHECK(script.events.back().line == 2);
    }

    SECTION("a seek inside a block uses the deck's track at each repetition") {
        const Script script = read_valid("track a sine 440 60\n"
                                         "track b sine 880 60 rate 48000\n"
                                         "0 load 1 a\n"
                                         "10 load 1 b\n"
                                         "5 repeat 2 every 10\n"
                                         "1 seek 1 2\n"
                                         "end repeat\n"
                                         "30 end\n");
        REQUIRE(script.events.size() == 4);
        CHECK(script.events.at(1) ==
              at(6, 6, TrackTime{.type = CommandType::Seek, .deck = 0, .seconds = 2, .track = 0}));
        CHECK(script.events.at(3) ==
              at(16, 6, TrackTime{.type = CommandType::Seek, .deck = 0, .seconds = 2, .track = 1}));
    }
}

namespace {

struct BadScript {
    std::string_view what;
    std::string text;
    std::size_t line;
};

/// A valid start: a track on line 1, loaded on deck 1 on line 2.
std::string with_prefix(std::string_view rest) {
    return std::string{"track s sine 440 10\n0 load 1 s\n"} + std::string{rest};
}

std::vector<BadScript> bad_scripts() {
    std::vector<BadScript> bad{
        {"an unknown command", with_prefix("1 jump 1\n9 end\n"), 3},
        {"an upper-case keyword", with_prefix("1 PLAY 1\n9 end\n"), 3},
        {"an unknown source", with_prefix("track t square 440 1\n9 end\n"), 3},
        {"a missing argument", with_prefix("1 play\n9 end\n"), 3},
        {"an extra argument", with_prefix("1 play 1 2\n9 end\n"), 3},
        {"a time that isn't a number", with_prefix("1x play 1\n9 end\n"), 3},
        {"a value that isn't a number", with_prefix("1 rate 1 fast\n9 end\n"), 3},
        {"nan", with_prefix("1 rate 1 nan\n9 end\n"), 3},
        {"inf", with_prefix("1 rate 1 inf\n9 end\n"), 3},
        {"+1", with_prefix("1 rate 1 +1\n9 end\n"), 3},
        {"an exponent", with_prefix("1 rate 1 1e3\n9 end\n"), 3},
        {"a leading point", with_prefix(".5 play 1\n9 end\n"), 3},
        {"a trailing point", with_prefix("5. play 1\n9 end\n"), 3},
        {"a negative time", with_prefix("-1 play 1\n9 end\n"), 3},
        {"a negative frequency", with_prefix("track t sine -440 1\n9 end\n"), 3},
        {"a fractional deck", with_prefix("1 play 1.5\n9 end\n"), 3},
        {"-inf for a fader", with_prefix("1 fader 1 -inf\n9 end\n"), 3},
        {"-inf for a rate", with_prefix("1 rate 1 -inf\n9 end\n"), 3},
        {"deck 0", with_prefix("1 play 0\n9 end\n"), 3},
        {"deck 5", with_prefix("1 play 5\n9 end\n"), 3},
        {"output channel 0", with_prefix("1 busmap 0 3\n9 end\n"), 3},
        {"output channel 9", with_prefix("1 busmap 1 9\n9 end\n"), 3},
        {"a bus map decode() rejects: one channel for both", with_prefix("1 busmap 1 1\n9 end\n"),
         3},
        {"a bus map decode() rejects: odd engine channels", with_prefix("1 busmap 2 4\n9 end\n"),
         3},
        {"a bad crossfader side", with_prefix("1 assign 1 left\n9 end\n"), 3},
        {"a bad pfl flag", with_prefix("1 pfl 1 yes\n9 end\n"), 3},
        {"an unknown track", with_prefix("1 load 2 nothere\n9 end\n"), 3},
        {"a duplicate track", with_prefix("track s sine 220 1\n9 end\n"), 3},
        {"a bad track name", with_prefix("track 9lives sine 440 1\n9 end\n"), 3},
        {"an unterminated path", with_prefix("track f file \"x.wav\n9 end\n"), 3},
        {"a sine at half the rate", with_prefix("track t sine 22050 1\n9 end\n"), 3},
        {"a sweep past half the rate", with_prefix("track t sweep 20 30000 1\n9 end\n"), 3},
        {"a signal too long", with_prefix("track t sine 1000 3601\n9 end\n"), 3},
        {"a signal of no length", with_prefix("track t noise 0\n9 end\n"), 3},
        {"clicks too fast", with_prefix("track t clicks 1001 10\n9 end\n"), 3},
        {"a rate too low", with_prefix("track t sine 1000 1 rate 7999\n9 end\n"), 3},
        {"a rate too high", with_prefix("track t noise 1 rate 384001\n9 end\n"), 3},
        {"a seek on a deck with nothing loaded", with_prefix("1 seek 2 3\n9 end\n"), 3},
        {"a cue point on a deck with nothing loaded", with_prefix("1 cuepoint 3 1\n9 end\n"), 3},
        {"a seek after an unload", with_prefix("1 unload 1\n2 seek 1 3\n9 end\n"), 4},
        {"a seek that runs before its deck's load", with_prefix("5 load 2 s\n1 seek 2 1\n9 end\n"),
         4},
        {"no end", with_prefix(""), 0},
        {"an empty script", "", 0},
        {"two ends", with_prefix("9 end\n10 end\n"), 4},
        {"an end at 0", "track s sine 440 10\n0 end\n", 2},
        {"an event at the end", with_prefix("9 play 1\n9 end\n"), 3},
        {"an event after an earlier end line", with_prefix("9 end\n10 play 1\n"), 4},
        {"a time over 86400 s", with_prefix("86400.5 play 1\n9 end\n"), 3},
        {"an end over 86400 s", with_prefix("86401 end\n"), 3},
        {"a repeat count of 0", with_prefix("1 repeat 0 every 1\n0 play 1\nend repeat\n9 end\n"),
         3},
        {"a repeat count over 100000",
         with_prefix("1 repeat 100001 every 0.0001\n0 play 1\nend repeat\n9 end\n"), 3},
        {"an offset at the period",
         with_prefix("1 repeat 2 every 1\n1 play 1\nend repeat\n9 end\n"), 4},
        {"a nested repeat",
         with_prefix("1 repeat 2 every 1\n0 repeat 2 every 0.1\n0 play 1\nend repeat\nend "
                     "repeat\n9 end\n"),
         4},
        {"a track line in a repeat",
         with_prefix("1 repeat 2 every 1\ntrack t sine 440 1\nend repeat\n9 end\n"), 4},
        {"an end in a repeat", with_prefix("1 repeat 2 every 1\n0 end\nend repeat\n9 end\n"), 4},
        {"a missing end repeat", with_prefix("9 end\n1 repeat 2 every 1\n0 play 1\n"), 4},
        {"an end repeat without a repeat", with_prefix("end repeat\n9 end\n"), 3},
        {"a repeat that expands past the most events",
         with_prefix("1 repeat 100000 every 0.0001\n0 pause 1\n0.00005 play 1\nend repeat\n20 "
                     "end\n"),
         6},
        {"an event past the most events",
         "0 repeat 100000 every 0.001\n0 crossfader 0.5\nend repeat\n150 crossfader 1\n200 end\n",
         4},
    };

    // 65 tracks: the 65th is the error.
    std::string tracks;
    for (std::size_t i = 0; i <= kMaxTracks; ++i) {
        tracks += "track t" + std::to_string(i) + " sine 440 1\n";
    }
    bad.push_back({"more than 64 tracks", tracks + "9 end\n", kMaxTracks + 1});

    // One byte over the limit, in a comment.
    std::string big = with_prefix("9 end\n#");
    big.append(kMaxScriptBytes - big.size() + 1, 'x');
    bad.push_back({"a script over 1 MiB", big, 0});
    return bad;
}

} // namespace

TEST_CASE("Script: errors name the line", "[harness]") {
    SECTION("each error, on its line") {
        for (const BadScript& bad : bad_scripts()) {
            CAPTURE(bad.what);
            const Read read = read_script(bad.text, kDirectory);
            REQUIRE(!read.has_value());
            CAPTURE(read.error().message);
            CHECK(read.error().line == bad.line);
            CHECK(!read.error().message.empty());
        }
    }

    SECTION("a script of exactly 1 MiB is read") {
        std::string exact = with_prefix("9 end\n#");
        exact.append(kMaxScriptBytes - exact.size(), 'x');
        REQUIRE(exact.size() == kMaxScriptBytes);
        const Read read = read_script(exact, kDirectory);
        CAPTURE(error_text(read));
        CHECK(read.has_value());
    }

    SECTION("-inf is a level") {
        const Read read = read_script(with_prefix("1 trim 1 -inf\n1 main -inf\n1 headphones "
                                                  "-inf\n9 end\n"),
                                      kDirectory);
        CAPTURE(error_text(read));
        CHECK(read.has_value());
    }
}
