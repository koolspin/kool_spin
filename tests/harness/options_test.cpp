// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for ks-harness's command line (specs/harness/spec.md
// requirement 2).

#include <ks/harness/options.h>

#include <ks/engine/miniaudio_backend.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ks::harness::Options;
using ks::harness::parse_options;

std::expected<Options, std::string> parse(std::initializer_list<std::string_view> args) {
    const std::vector<std::string_view> list{args};
    return parse_options(list);
}

bool refused(std::initializer_list<std::string_view> args) {
    const auto parsed = parse(args);
    return !parsed && parsed.error().starts_with("ks-harness: ") &&
           parsed.error().find('\n') == std::string::npos;
}

} // namespace

TEST_CASE("usage() starts with the usage line", "[harness][options]") {
    const std::string text = ks::harness::usage();
    CHECK(text.starts_with("Usage: ks-harness [options] <script>\n"));
}

TEST_CASE("parse_options() reads a script and leaves the rest empty", "[harness][options]") {
    const auto parsed = parse({"a.harness"});
    REQUIRE(parsed);
    CHECK(parsed->script == std::filesystem::path{"a.harness"});
    CHECK_FALSE(parsed->out);
    CHECK_FALSE(parsed->backend);
    CHECK_FALSE(parsed->device);
    CHECK_FALSE(parsed->rate);
    CHECK_FALSE(parsed->channels);
    CHECK_FALSE(parsed->period);
    CHECK_FALSE(parsed->list_devices);
    CHECK_FALSE(parsed->check);
    CHECK_FALSE(parsed->help);
}

TEST_CASE("parse_options() reads every option", "[harness][options]") {
    SECTION("device mode") {
        const auto parsed = parse({"--backend", "null", "--device", "3", "--rate", "44100",
                                   "--channels", "8", "--period", "4096", "--check", "s"});
        REQUIRE(parsed);
        CHECK(parsed->backend == ks::engine::MiniaudioBackends::Null);
        CHECK(parsed->device == std::size_t{3});
        CHECK(parsed->rate == std::uint32_t{44'100});
        CHECK(parsed->channels == std::uint32_t{8});
        CHECK(parsed->period == std::uint32_t{4'096});
        CHECK(parsed->check);
        CHECK(parsed->script == std::filesystem::path{"s"});
    }
    SECTION("device mode accepts 0") {
        const auto parsed = parse({"--rate", "0", "--channels", "0", "--period", "0", "s"});
        REQUIRE(parsed);
        CHECK(parsed->rate == std::uint32_t{0});
        CHECK(parsed->channels == std::uint32_t{0});
        CHECK(parsed->period == std::uint32_t{0});
    }
    SECTION("file mode") {
        const auto parsed =
            parse({"s", "--out", "o.wav", "--rate", "8000", "--channels", "2", "--period", "1"});
        REQUIRE(parsed);
        CHECK(parsed->out == std::filesystem::path{"o.wav"});
        CHECK(parsed->rate == std::uint32_t{8'000});
        CHECK(parsed->channels == std::uint32_t{2});
        CHECK(parsed->period == std::uint32_t{1});
    }
    SECTION("--list-devices needs no script") {
        const auto parsed = parse({"--list-devices", "--backend", "null"});
        REQUIRE(parsed);
        CHECK(parsed->list_devices);
        CHECK_FALSE(parsed->script);
    }
}

TEST_CASE("parse_options() stops at --help", "[harness][options]") {
    for (const auto args :
         {std::initializer_list<std::string_view>{"--help"},
          std::initializer_list<std::string_view>{"--help", "--bogus"},
          std::initializer_list<std::string_view>{"--out", "o", "-h", "--rate"}}) {
        const auto parsed = parse(args);
        REQUIRE(parsed);
        CHECK(parsed->help);
    }
}

TEST_CASE("parse_options() refuses bad values", "[harness][options]") {
    CHECK(refused({"--rate", "7999", "s"}));
    CHECK(refused({"--rate", "384001", "s"}));
    CHECK(refused({"--rate", "fast", "s"}));
    CHECK(refused({"--rate", "+48000", "s"}));
    CHECK(refused({"--rate", "-48000", "s"}));
    CHECK(refused({"--rate", "48000.0", "s"}));
    CHECK(refused({"--rate", " 48000", "s"}));
    CHECK(refused({"--rate", "", "s"}));
    CHECK(refused({"--rate", "99999999999999999999", "s"}));
    CHECK(refused({"--channels", "1", "s"}));
    CHECK(refused({"--channels", "9", "s"}));
    CHECK(refused({"--period", "4097", "s"}));
    CHECK(refused({"--device", "-1", "s"}));
    CHECK(refused({"--device", "x", "s"}));
    CHECK(refused({"--backend", "wasapi", "s"}));
    CHECK(refused({"--out", "", "s"}));
    CHECK(refused({"s", "--rate"}));
}

TEST_CASE("parse_options() refuses bad combinations", "[harness][options]") {
    CHECK(refused({"--bogus", "s"}));
    CHECK(refused({"-"}));
    CHECK(refused({"a", "b"}));
    CHECK(refused({}));
    CHECK(refused({"--check"}));
    CHECK(refused({"--backend", "null"}));
    CHECK(refused({"--out", "o.wav", "--backend", "null", "s"}));
    CHECK(refused({"--device", "0", "--out", "o.wav", "s"}));
    CHECK(refused({"--out", "o.wav", "--rate", "0", "s"}));
    CHECK(refused({"--channels", "0", "--out", "o.wav", "s"}));
    CHECK(refused({"--out", "o.wav", "--period", "0", "s"}));
}
