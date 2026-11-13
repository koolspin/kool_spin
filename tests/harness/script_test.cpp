// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the script reader (specs/harness/spec.md requirements 4 to
// 16), beyond the acceptance tests.

#include <ks/harness/script.h>

#include <ks/engine/command.h>
#include <ks/ipc/command.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <variant>

namespace {

using ks::engine::Command;
using ks::harness::command_for;
using ks::harness::Load;
using ks::harness::read_script;
using ks::harness::Script;
using ks::harness::ScriptError;
using ks::harness::TrackTime;
using ks::ipc::CommandType;

const std::filesystem::path kDirectory{"/s"};

std::size_t error_line(const std::string& text) {
    const std::expected<Script, ScriptError> read = read_script(text, kDirectory);
    REQUIRE(!read.has_value());
    CAPTURE(read.error().message);
    CHECK(!read.error().message.empty());
    return read.error().line;
}

TEST_CASE("script: a load may name a track defined later") {
    const auto read =
        read_script("0 load 1 b\ntrack a noise 1\ntrack b noise 1\n1 end\n", kDirectory);
    REQUIRE(read.has_value());
    REQUIRE(read->events.size() == 1);
    CHECK(std::get<Load>(read->events.front().action) == Load{.deck = 0, .track = 1});
}

TEST_CASE("script: events sort by time, ties in line order") {
    const auto read = read_script("2 crossfader 0.5\n1 crossfader 0.25\n1 crossfader 0.75\n"
                                  "0.5 cuemix 1\n3 end\n",
                                  kDirectory);
    REQUIRE(read.has_value());
    REQUIRE(read->events.size() == 4);
    CHECK(read->events.at(0).line == 4);
    CHECK(read->events.at(1).line == 2);
    CHECK(read->events.at(2).line == 3);
    CHECK(read->events.at(3).line == 1);
    CHECK(std::get<Command>(read->events.at(1).action) ==
          Command{.type = CommandType::SetCrossfader, .value = 0.25});
}

TEST_CASE("script: line-local errors") {
    CHECK(error_line("1 end\n0 crossfader \"0.5\"\n") == 2);
    CHECK(error_line("1 end\ntrack a file x.wav\n") == 2);
    CHECK(error_line("1 end\ntrack a file \"x.wav\" \"y.wav\"\n") == 2);
    CHECK(error_line("1 end\ntrack a sine 440 1 rate\n") == 2);
    CHECK(error_line("1 end\ntrack a sine 440 1 rate 48000 extra\n") == 2);
    CHECK(error_line("1 end\ntrack a clicks 0 1\n") == 2);
    CHECK(error_line("1 end\ntrack a sweep 0 100 1\n") == 2);
    CHECK(error_line("1 end\nend\n") == 2);
    CHECK(error_line("1 end extra\n") == 1);
    CHECK(error_line("1 end\n0\n") == 2);
}

TEST_CASE("script: the end and the time limit") {
    CHECK(read_script("86400 end\n", kDirectory).has_value());
    CHECK(read_script("0 cuemix 0\n0.001 end\n", kDirectory).has_value());
    CHECK(error_line("1 cuemix 0\n1 end\n") == 1);
}

TEST_CASE("script: seek and cue point follow the deck's track in run order") {
    const auto read = read_script("track a noise 1\n"
                                  "track b file \"b.wav\"\n"
                                  "3 seek 1 0.5\n"
                                  "2 load 1 b\n"
                                  "0 load 1 a\n"
                                  "1 cuepoint 1 0\n"
                                  "2 seek 1 86400\n"
                                  "4 end\n",
                                  kDirectory);
    REQUIRE(read.has_value());
    REQUIRE(read->events.size() == 5);
    CHECK(std::get<TrackTime>(read->events.at(1).action) ==
          TrackTime{.type = CommandType::SetCuePoint, .deck = 0, .seconds = 0, .track = 0});
    // Line 7 comes after the load on line 4 at the same time.
    CHECK(std::get<TrackTime>(read->events.at(3).action).track == 1);
    CHECK(std::get<TrackTime>(read->events.at(4).action) ==
          TrackTime{.type = CommandType::Seek, .deck = 0, .seconds = 0.5, .track = 1});
}

TEST_CASE("script: seek and cue point errors") {
    const std::string tracks = "track a noise 1\n0 load 1 a\n";
    CHECK(error_line(tracks + "1 seek 1\n9 end\n") == 3);
    CHECK(error_line(tracks + "1 seek 1 -1\n9 end\n") == 3);
    CHECK(error_line(tracks + "1 cuepoint 1 86400.5\n9 end\n") == 3);
    CHECK(error_line(tracks + "1 seek 0 1\n9 end\n") == 3);
    // At the same time, a seek on an earlier line runs before the load.
    CHECK(error_line("track a noise 1\n1 seek 1 0\n1 load 1 a\n9 end\n") == 2);
    // The first in run order, not in script order.
    CHECK(error_line("track a noise 1\n5 seek 2 0\n2 seek 3 0\n9 end\n") == 3);
}

TEST_CASE("script: command_for() converts seconds to frames") {
    const TrackTime seek{.type = CommandType::Seek, .deck = 2, .seconds = 1.5, .track = 0};
    CHECK(command_for(seek, 8'000) ==
          Command{.type = CommandType::Seek, .deck = 2, .value = 12'000});
    const TrackTime cue{.type = CommandType::SetCuePoint, .deck = 0, .seconds = 86'400};
    CHECK(command_for(cue, 384'000) ==
          Command{.type = CommandType::SetCuePoint, .deck = 0, .value = 86'400.0 * 384'000.0});
}

TEST_CASE("script: a load inside a repeat block resolves at every repetition") {
    const auto read = read_script("0 repeat 3 every 1\n0 load 2 b\n0.5 unload 2\nend repeat\n"
                                  "track b noise 1\n5 end\n",
                                  kDirectory);
    REQUIRE(read.has_value());
    REQUIRE(read->events.size() == 6);
    for (std::size_t k = 0; k < 3; ++k) {
        const auto& load = read->events.at(2 * k);
        CHECK(load.time == static_cast<double>(k));
        CHECK(load.line == 2);
        CHECK(std::get<Load>(load.action) == Load{.deck = 1, .track = 0});
        CHECK(read->events.at((2 * k) + 1).line == 3);
    }
}

TEST_CASE("script: an empty repeat block adds nothing") {
    const auto read = read_script("1 repeat 5 every 1\nend repeat\n9 end\n", kDirectory);
    REQUIRE(read.has_value());
    CHECK(read->events.empty());
}

TEST_CASE("script: repeat block errors") {
    // The repeat line's own form.
    CHECK(error_line("1 repeat 2\n9 end\n") == 1);
    CHECK(error_line("1 repeat 2 each 1\n0 play 1\nend repeat\n9 end\n") == 1);
    CHECK(error_line("1 repeat 2 every 0\n0 play 1\nend repeat\n9 end\n") == 1);
    CHECK(error_line("1 repeat 1.5 every 1\n0 play 1\nend repeat\n9 end\n") == 1);
    // A repetition past 86,400 s names the line inside.
    CHECK(error_line("86000 repeat 3 every 200\n0 play 1\n150 pause 1\nend repeat\n"
                     "86400 end\n") == 3);
    // A repetition at or after the end names the line inside.
    CHECK(error_line("1 repeat 3 every 2\n0 play 1\nend repeat\n5 end\n") == 2);
    // One more event than allowed, counted before expanding.
    CHECK(error_line("0 crossfader 0\n0 repeat 100000 every 0.001\n0 crossfader 0.5\n"
                     "end repeat\n200 end\n") == 4);
    // The missing end repeat is reported on its repeat line, wherever the
    // file ends.
    CHECK(error_line("1 end\n2 play 1\n0.5 repeat 2 every 0.1\n0 play 1\n") == 3);
}

} // namespace
