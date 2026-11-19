// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/play_app/options.h>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <filesystem>
#include <initializer_list>
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
