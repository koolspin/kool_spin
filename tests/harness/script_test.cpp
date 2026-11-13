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
using ks::harness::Load;
using ks::harness::read_script;
using ks::harness::Script;
using ks::harness::ScriptError;
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

} // namespace
