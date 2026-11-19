// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/client/session.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>

// Session's connection needs a supervisor, which unit tests don't start: the
// acceptance tests in tests/acceptance/ks-play/ cover it. These cover what
// fails before anything listens.

namespace {

namespace client = ks::client;

/// A directory that doesn't exist, so nothing listens in it.
std::filesystem::path nowhere() {
    return std::filesystem::temp_directory_path() / "ks-client-session-test-nowhere" / "missing";
}

} // namespace

TEST_CASE("Session::connect(): names", "[client]") {
    SECTION("an empty name is refused before connecting") {
        const client::Result<client::Session> result = client::Session::connect("", nowhere());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == client::ErrorKind::InvalidArgument);
    }

    SECTION("a name of 65 bytes is refused before connecting") {
        const client::Result<client::Session> result =
            client::Session::connect(std::string(client::kMaxNameSize + 1, 'n'), nowhere());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == client::ErrorKind::InvalidArgument);
    }

    SECTION("a name of 64 bytes gets as far as connecting") {
        const client::Result<client::Session> result =
            client::Session::connect(std::string(client::kMaxNameSize, 'n'), nowhere());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == client::ErrorKind::NotReachable);
    }
}

TEST_CASE("Session::connect(): nothing to connect to", "[client]") {
    SECTION("a missing runtime directory") {
        const client::Result<client::Session> result =
            client::Session::connect("tester", nowhere(), client::Milliseconds{100});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == client::ErrorKind::NotReachable);
        CHECK_FALSE(result.error().text.empty());
    }

    SECTION("connect_as() too") {
        const client::Result<client::Session> result =
            client::Session::connect_as("tester", nowhere(), 0, client::Milliseconds{100});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == client::ErrorKind::NotReachable);
    }

    SECTION("returns at once, without waiting for its timeout") {
        const auto start = std::chrono::steady_clock::now();
        const client::Result<client::Session> result =
            client::Session::connect("tester", nowhere(), client::Milliseconds{5'000});
        CHECK_FALSE(result.has_value());
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
    }
}
