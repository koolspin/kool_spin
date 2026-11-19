// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/play_app/options.h>

#include <ks/client/command.h>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <expected>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

namespace app = ks::play_app;

std::expected<app::Invocation, std::string> parse(std::initializer_list<std::string_view> args) {
    const std::vector<std::string_view> storage{args};
    return app::parse(storage, "/work/dir", "/run/user/1000/kool_spin");
}

} // namespace

TEST_CASE("ks-play: the global options", "[play_app][options]") {
    SECTION("defaults") {
        const auto parsed = parse({"ping"});
        REQUIRE(parsed.has_value());
        CHECK(parsed->runtime_dir == std::filesystem::path{"/run/user/1000/kool_spin"});
        CHECK(parsed->name == "ks-play");
        CHECK(std::holds_alternative<app::Ping>(parsed->request));
    }
    SECTION("--runtime-dir and --name, in either order") {
        for (const auto& parsed :
             {parse({"--runtime-dir", "/tmp/ks", "--name", "deck-a", "stop"}),
              parse({"--name", "deck-a", "--runtime-dir", "/tmp/ks", "stop"})}) {
            REQUIRE(parsed.has_value());
            CHECK(parsed->runtime_dir == std::filesystem::path{"/tmp/ks"});
            CHECK(parsed->name == "deck-a");
            CHECK(std::holds_alternative<app::Stop>(parsed->request));
        }
    }
    SECTION("a name of 64 bytes") {
        const std::string name(64, 'n');
        const auto parsed = parse({"--name", name, "ping"});
        REQUIRE(parsed.has_value());
        CHECK(parsed->name == name);
    }
    SECTION("--help") {
        for (const auto& parsed : {parse({"--help"}), parse({"--name", "x", "--help"})}) {
            REQUIRE(parsed.has_value());
            CHECK(std::holds_alternative<app::Help>(parsed->request));
        }
        CHECK(app::usage().starts_with("usage: ks-play"));
    }
}

TEST_CASE("ks-play: load", "[play_app][options]") {
    SECTION("an absolute path") {
        const auto parsed = parse({"load", "2", "/music/a.flac"});
        REQUIRE(parsed.has_value());
        const auto* const load = std::get_if<app::Load>(&parsed->request);
        REQUIRE(load != nullptr);
        CHECK(load->deck == 2);
        CHECK(load->path == "/music/a.flac");
        CHECK_FALSE(load->wait);
    }
    SECTION("a relative path, made absolute lexically") {
        const auto parsed = parse({"load", "0", "../music/./a.flac", "--wait"});
        REQUIRE(parsed.has_value());
        const auto* const load = std::get_if<app::Load>(&parsed->request);
        REQUIRE(load != nullptr);
        CHECK(load->deck == 0);
        CHECK(load->path == "/work/music/a.flac");
        CHECK(load->wait);
    }
    SECTION("a path that looks like an option") {
        const auto parsed = parse({"load", "3", "--wait"});
        REQUIRE(parsed.has_value());
        const auto* const load = std::get_if<app::Load>(&parsed->request);
        REQUIRE(load != nullptr);
        CHECK(load->path == "/work/dir/--wait");
        CHECK_FALSE(load->wait);
    }
}

TEST_CASE("ks-play: unload, stop, ping", "[play_app][options]") {
    const auto unload = parse({"unload", "3"});
    REQUIRE(unload.has_value());
    REQUIRE(std::holds_alternative<app::Unload>(unload->request));
    CHECK(std::get<app::Unload>(unload->request).deck == 3);

    const auto stop = parse({"stop"});
    REQUIRE(stop.has_value());
    CHECK(std::holds_alternative<app::Stop>(stop->request));
}

TEST_CASE("ks-play: status", "[play_app][options]") {
    const auto plain = parse({"status"});
    REQUIRE(plain.has_value());
    REQUIRE(std::holds_alternative<app::Status>(plain->request));
    CHECK_FALSE(std::get<app::Status>(plain->request).watch);
    CHECK(std::get<app::Status>(plain->request).interval_ms == 500);

    for (const auto& parsed : {parse({"status", "--watch", "--interval", "50"}),
                               parse({"status", "--interval", "50", "--watch"})}) {
        REQUIRE(parsed.has_value());
        REQUIRE(std::holds_alternative<app::Status>(parsed->request));
        CHECK(std::get<app::Status>(parsed->request).watch);
        CHECK(std::get<app::Status>(parsed->request).interval_ms == 50);
    }

    const auto longest = parse({"status", "--interval", "60000"});
    REQUIRE(longest.has_value());
    CHECK(std::get<app::Status>(longest->request).interval_ms == 60'000);
}

namespace {

/// The command a control command line builds.
ks::client::Command control(std::initializer_list<std::string_view> args) {
    const auto parsed = parse(args);
    REQUIRE(parsed.has_value());
    const auto* const built = std::get_if<app::Control>(&parsed->request);
    REQUIRE(built != nullptr);
    return built->command;
}

double real_of(const ks::client::Command& command) {
    return std::bit_cast<double>(command.payload[0]);
}

} // namespace

TEST_CASE("ks-play: the control commands", "[play_app][options]") {
    namespace client = ks::client;
    constexpr double kNegativeInfinity = -std::numeric_limits<double>::infinity();

    CHECK(control({"play", "3"}) == client::set_playing(3, true));
    CHECK(control({"pause", "0"}) == client::set_playing(0, false));
    CHECK(control({"rate", "1", "1.02"}) == client::set_rate(1, 1.02));
    CHECK(control({"rate", "0", "-1"}) == client::set_rate(0, -1.0));
    CHECK(control({"seek", "0", "22050"}) == client::seek(0, 22'050.0));
    CHECK(control({"seek", "2", "1.5e3"}) == client::seek(2, 1'500.0));
    CHECK(control({"cue-point", "0", "11025"}) == client::set_cue_point(0, 11'025.0));
    CHECK(control({"trim", "2", "-3"}) == client::set_trim(2, -3.0));
    CHECK(real_of(control({"trim", "2", "-inf"})) == kNegativeInfinity);
    CHECK(control({"fader", "3", "0.5"}) == client::set_fader(3, 0.5));
    CHECK(control({"assign", "0", "a"}) == client::set_crossfader_assign(0, 0));
    CHECK(control({"assign", "1", "b"}) == client::set_crossfader_assign(1, 1));
    CHECK(control({"assign", "2", "thru"}) == client::set_crossfader_assign(2, 2));
    CHECK(control({"channel-cue", "0", "on"}) == client::set_channel_cue(0, true));
    CHECK(control({"channel-cue", "3", "off"}) == client::set_channel_cue(3, false));
    CHECK(control({"crossfader", "0.25"}) == client::set_crossfader(0.25));
    CHECK(control({"curve", "0.5"}) == client::set_crossfader_curve(0.5));
    CHECK(control({"main-level", "-6"}) == client::set_main_level(-6.0));
    CHECK(real_of(control({"main-level", "-inf"})) == kNegativeInfinity);
    CHECK(control({"headphone-level", "-9"}) == client::set_headphone_level(-9.0));
    CHECK(real_of(control({"headphone-level", "-inf"})) == kNegativeInfinity);
    CHECK(control({"cue-mix", "0.75"}) == client::set_cue_mix(0.75));
    CHECK(control({"bus-map", "2", "4"}) == client::set_bus_map(2, 4));
    CHECK(control({"bus-map", "1", "4294967295"}) == client::set_bus_map(1, 4'294'967'295U));
}

TEST_CASE("ks-play: control commands' usage errors", "[play_app][options]") {
    const std::vector<std::vector<std::string_view>> bad{
        {"play"},
        {"play", "0", "1"},
        {"play", "4"},
        {"play", "-1"},
        {"play", "1.5"},
        {"play", " 1"},
        {"play", ""},
        {"pause"},
        {"pause", "0", "1"},
        {"rate", "0"},
        {"rate", "0", "1", "2"},
        {"rate", "4", "1"},
        {"rate", "0", "1x"},
        {"rate", "0", "0x10"},
        {"rate", "0", "inf"},
        {"rate", "0", "nan"},
        {"rate", "0", "-inf"},
        {"rate", "0", "1e999"},
        {"rate", "0", ""},
        {"seek", "0"},
        {"seek", "0", "-inf"},
        {"seek", "0", "1", "2"},
        {"cue-point", "0"},
        {"cue-point", "0", "nan"},
        {"trim", "0"},
        {"trim", "0", "inf"},
        {"trim", "0", "+inf"},
        {"trim", "0", "-3", "1"},
        {"fader", "0"},
        {"fader", "0", "-inf"},
        {"fader", "4", "0.5"},
        {"assign", "0"},
        {"assign", "0", "a", "b"},
        {"assign", "0", "c"},
        {"assign", "0", "A"},
        {"assign", "0", "2"},
        {"assign", "4", "a"},
        {"channel-cue", "0"},
        {"channel-cue", "0", "maybe"},
        {"channel-cue", "0", "1"},
        {"channel-cue", "0", "on", "off"},
        {"crossfader"},
        {"crossfader", "0.5", "1"},
        {"crossfader", "-inf"},
        {"curve"},
        {"curve", "nan"},
        {"main-level"},
        {"main-level", "inf"},
        {"main-level", "-6", "1"},
        {"headphone-level"},
        {"headphone-level", "nan"},
        {"cue-mix"},
        {"cue-mix", "-inf"},
        {"bus-map", "2"},
        {"bus-map", "2", "4", "6"},
        {"bus-map", "2.5", "4"},
        {"bus-map", "2", "-4"},
        {"bus-map", "2", "4294967296"},
        {"bus-map", "0x2", "4"},
    };
    for (const std::vector<std::string_view>& args : bad) {
        CAPTURE(args);
        const auto parsed = app::parse(args, "/work", "/run");
        REQUIRE(!parsed.has_value());
        CHECK(parsed.error().starts_with(args.front()));
        CHECK(parsed.error().find('\n') == std::string::npos);
    }
}

TEST_CASE("ks-play: usage errors", "[play_app][options]") {
    const std::string long_name(65, 'n');
    const std::vector<std::vector<std::string_view>> bad{
        {},
        {"frobnicate"},
        {"load", "0"},
        {"load"},
        {"load", "0", "/a.flac", "extra"},
        {"load", "0", "/a.flac", "--wait", "extra"},
        {"load", "0", ""},
        {"load", "4", "/a.flac"},
        {"unload"},
        {"unload", "0", "1"},
        {"unload", "4"},
        {"unload", "-1"},
        {"unload", "1.5"},
        {"unload", " 1"},
        {"unload", ""},
        {"status", "extra"},
        {"status", "--interval"},
        {"status", "--interval", "49"},
        {"status", "--interval", "60001"},
        {"status", "--interval", "x"},
        {"status", "--interval", "-50"},
        {"status", "--interval", "99999999999999999999999"},
        {"status", "--watch", "--watch"},
        {"status", "--interval", "50", "--interval", "60"},
        {"stop", "now"},
        {"ping", "now"},
        {"--runtime-dir", "/a", "--runtime-dir", "/b", "ping"},
        {"--runtime-dir"},
        {"--runtime-dir", "", "ping"},
        {"--name", long_name, "ping"},
        {"--name", "", "ping"},
        {"--name"},
        {"--name", "a", "--name", "b", "ping"},
        {"--unknown", "ping"},
        {"--runtime-dir", "/a"},
        {"ping", "--help"},
    };
    for (const std::vector<std::string_view>& args : bad) {
        CAPTURE(args);
        const auto parsed = app::parse(args, "/work", "/run");
        // Not REQUIRE_FALSE or CHECK_FALSE: in a loop, clang-analyzer takes
        // the flags they combine for an enum value out of range.
        REQUIRE(!parsed.has_value());
        CHECK(!parsed.error().empty());
        CHECK(parsed.error().find('\n') == std::string::npos);
    }
}
