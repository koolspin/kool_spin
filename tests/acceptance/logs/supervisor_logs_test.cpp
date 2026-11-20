// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/logs/spec.md: kool_spin's session folders and
// log files, end to end (requirements 9 to 15 and 19). Each test runs
// kool_spin in its own runtime directory, through /bin/sh so its standard
// error goes to a file, with ks-engine on miniaudio's null backend.

#include <ks/ipc/connection.h>
#include <ks/ipc/messages.h>

#include "logs_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using ks::client_rings::test::Client;
using ks::client_rings::test::join;
using ks::client_rings::test::open_ring;
using ks::client_rings::test::OpenedRing;
using ks::client_rings::test::OpenFailure;
using ks::ipc::ErrorCode;
using ks::ipc::Message;
using ks::logs::test::find_line;
using ks::logs::test::header_tail;
using ks::logs::test::Line;
using ks::logs::test::LoggedKoolSpin;
using ks::logs::test::mode_of;
using ks::logs::test::read_lines;
using ks::logs::test::session_folders;
using ks::logs::test::wait_for_line;
using ks::logs::test::well_formed;
using ks::logs::test::write_file;
using ks::processes::test::eventually;
using ks::processes::test::Milliseconds;
namespace msg = ks::ipc::msg;

/// Joins `kool_spin` as `name`, or fails the test.
Client must_join(const LoggedKoolSpin& kool_spin, const std::string& name) {
    std::expected<Client, std::string> client = join(kool_spin.dir(), name);
    REQUIRE(client.has_value());
    return std::move(*client);
}

/// Opens a ring for `client` with request ID 2, or fails the test.
OpenedRing must_open(Client& client) {
    std::expected<OpenedRing, OpenFailure> opened = open_ring(client.connection, 2);
    REQUIRE(opened.has_value());
    return std::move(*opened);
}

/// The session folder, or fails the test.
std::filesystem::path must_find_session(const LoggedKoolSpin& kool_spin,
                                        const std::filesystem::path& log_dir) {
    const std::optional<std::filesystem::path> session = kool_spin.session(log_dir);
    REQUIRE(session.has_value());
    return session.value_or(std::filesystem::path{});
}

/// Waits up to 2 s for a line of `file` (standard error) that starts with
/// `prefix` and contains `phrase`.
bool stderr_has(const std::filesystem::path& file, std::string_view prefix,
                std::string_view phrase) {
    return eventually(
        [&] {
            return std::ranges::any_of(read_lines(file), [&](const std::string& line) {
                return line.starts_with(prefix) && line.find(phrase) != std::string::npos;
            });
        },
        ks::logs::test::kLineWait);
}

/// The wall-clock time a header's "session started" gives, or nothing if it
/// can't be read.
std::optional<std::chrono::system_clock::time_point> header_start(const std::string& header) {
    static const std::regex kPattern{
        R"(session started (\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})\.(\d{6}) ([+-])(\d{2}):(\d{2});)"};
    std::smatch match;
    if (!std::regex_search(header, match, kPattern)) {
        return std::nullopt;
    }
    const auto number = [&match](std::size_t group) { return std::stoi(match[group].str()); };
    const std::chrono::sys_days date = std::chrono::year{number(1)} /
                                       std::chrono::month{static_cast<unsigned>(number(2))} /
                                       std::chrono::day{static_cast<unsigned>(number(3))};
    const auto local = date + std::chrono::hours{number(4)} + std::chrono::minutes{number(5)} +
                       std::chrono::seconds{number(6)} + std::chrono::microseconds{number(7)};
    const int sign = match[8].str() == "-" ? -1 : 1;
    const std::chrono::minutes offset =
        sign * (std::chrono::hours{number(9)} + std::chrono::minutes{number(10)});
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(local - offset);
}

} // namespace

TEST_CASE("Logs: a session folder", "[supervisor]") {
    LoggedKoolSpin kool_spin;
    const auto launched = std::chrono::system_clock::now();
    kool_spin.start();
    Client client = must_join(kool_spin, "first");

    const std::filesystem::path session = must_find_session(kool_spin, kool_spin.logs());
    CHECK(session_folders(kool_spin.logs()).size() == 1);
    CHECK(session.filename().string().ends_with(std::format("_{}", kool_spin.process().pid())));
    CHECK(mode_of(session) == 0700U);
    const std::filesystem::path own = session / "kool_spin.log";
    const std::filesystem::path engine = session / "ks-engine.log";
    CHECK(mode_of(own) == 0600U);
    CHECK(mode_of(engine) == 0600U);

    REQUIRE(ks::processes::test::send(client.connection,
                                      Message{.request_id = 9, .body = msg::Stop{}}));
    CHECK(kool_spin.process().wait() == 0);

    const std::vector<std::string> own_lines = read_lines(own);
    const std::vector<std::string> engine_lines = read_lines(engine);
    CHECK(well_formed(own_lines, "kool_spin"));
    CHECK(well_formed(engine_lines, "ks-engine"));
    REQUIRE(!own_lines.empty());
    REQUIRE(!engine_lines.empty());
    const std::optional<std::string> own_header = header_tail(own_lines.front(), "kool_spin");
    const std::optional<std::string> engine_header = header_tail(engine_lines.front(), "ks-engine");
    REQUIRE(own_header.has_value());
    CHECK(own_header == engine_header);
    CHECK(find_line(engine_lines, "info", "ready").has_value());
    CHECK(find_line(engine_lines, "info", "stopped").has_value());

    const std::optional<std::chrono::system_clock::time_point> started =
        header_start(own_lines.front());
    REQUIRE(started.has_value());
    const auto apart = started.value_or(launched) - launched;
    CHECK(apart < std::chrono::seconds{5});
    CHECK(apart > -std::chrono::seconds{5});
}

TEST_CASE("Logs: shared timestamps", "[supervisor]") {
    LoggedKoolSpin kool_spin;
    kool_spin.start();
    Client client = must_join(kool_spin, "timer");
    const OpenedRing opened = must_open(client);
    REQUIRE(opened.slot == 0);

    const std::filesystem::path session = must_find_session(kool_spin, kool_spin.logs());
    const std::optional<Line> asked =
        wait_for_line(session / "kool_spin.log", "info", "client 'timer' asked for a ring");
    const std::optional<Line> attached =
        wait_for_line(session / "ks-engine.log", "info", "attached a ring in slot 0");
    const std::optional<Line> opened_line =
        wait_for_line(session / "kool_spin.log", "info", "client 'timer' opened a ring in slot 0");
    REQUIRE(asked.has_value());
    REQUIRE(attached.has_value());
    REQUIRE(opened_line.has_value());
    const std::string a = asked.value_or(Line{}).time;
    const std::string b = attached.value_or(Line{}).time;
    const std::string c = opened_line.value_or(Line{}).time;
    CAPTURE(a, b, c);
    CHECK(a <= b);
    CHECK(b <= c);
}

TEST_CASE("Logs: the engine's last lines outlive it", "[supervisor]") {
    LoggedKoolSpin kool_spin;
    kool_spin.start();
    const pid_t engine = kool_spin.engine();
    REQUIRE(engine > 0);
    Client client = must_join(kool_spin, "doomed");
    const OpenedRing opened = must_open(client);
    REQUIRE(::kill(engine, SIGKILL) == 0);
    CHECK(opened.slot == 0);

    // The supervisor restarts the engine (specs/engine-restart/spec.md
    // requirement 1) and carries on, so there's no exit to wait for.
    const std::filesystem::path session = must_find_session(kool_spin, kool_spin.logs());
    CHECK(eventually([&] {
        return find_line(read_lines(session / "kool_spin.log"), "error",
                         "ks-engine was killed by signal 9")
            .has_value();
    }));
    const std::vector<std::string> engine_lines = read_lines(session / "ks-engine.log");
    CHECK(well_formed(engine_lines, "ks-engine"));
    CHECK(find_line(engine_lines, "info", "attached a ring in slot 0").has_value());
}

TEST_CASE("Logs: keeps the newest ten sessions", "[supervisor]") {
    LoggedKoolSpin kool_spin;
    const std::filesystem::path logs = kool_spin.logs();
    std::filesystem::create_directories(logs);
    const auto old_session = [&logs](int day) {
        return logs / std::format("2020-01-{:02}_00-00-00_1", day);
    };
    for (int day = 1; day <= 12; ++day) {
        std::filesystem::create_directory(old_session(day));
        write_file(old_session(day) / "kool_spin.log", "# kool_spin log\n");
    }
    write_file(old_session(2) / "readme.txt", "keep me\n");
    std::filesystem::create_directory(logs / "notes");
    write_file(logs / "notes" / "keep.txt", "keep me\n");
    write_file(logs / "2020-01-01_00-00-00_1.log", "keep me\n");

    kool_spin.start();
    const std::filesystem::path session = must_find_session(kool_spin, logs);

    for (int day = 4; day <= 12; ++day) {
        CAPTURE(day);
        CHECK(std::filesystem::exists(old_session(day) / "kool_spin.log"));
    }
    CHECK(!std::filesystem::exists(old_session(1)));
    CHECK(!std::filesystem::exists(old_session(3)));
    CHECK(std::filesystem::exists(old_session(2) / "readme.txt"));
    CHECK(!std::filesystem::exists(old_session(2) / "kool_spin.log"));
    CHECK(std::filesystem::exists(logs / "notes" / "keep.txt"));
    CHECK(std::filesystem::exists(logs / "2020-01-01_00-00-00_1.log"));
    CHECK(std::filesystem::exists(session / "kool_spin.log"));
    // The nine kept, 2020-01-02 with its readme, and the new one.
    CHECK(session_folders(logs).size() == 11);
}

TEST_CASE("Logs: the default folder", "[supervisor]") {
    LoggedKoolSpin kool_spin;
    const std::filesystem::path home = kool_spin.dir() / "home";
    const std::filesystem::path state = kool_spin.dir() / "state";
    std::filesystem::create_directories(home);
    const std::string home_is = "HOME=" + home.string();
    const std::string state_is = "XDG_STATE_HOME=" + state.string();

#ifdef __linux__
    SECTION("XDG_STATE_HOME") {
        kool_spin.start(std::nullopt, {home_is, state_is});
        const std::filesystem::path logs = state / "kool_spin" / "logs";
        const std::filesystem::path session = must_find_session(kool_spin, logs);
        CHECK(std::filesystem::exists(session / "kool_spin.log"));
    }

    SECTION("HOME") {
        kool_spin.start(std::nullopt, {"-u", "XDG_STATE_HOME", home_is});
        const std::filesystem::path logs = home / ".local" / "state" / "kool_spin" / "logs";
        const std::filesystem::path session = must_find_session(kool_spin, logs);
        CHECK(std::filesystem::exists(session / "kool_spin.log"));
    }
#else
    kool_spin.start(std::nullopt, {home_is, state_is});
    const std::filesystem::path logs = home / "Library" / "Logs" / "kool_spin";
    const std::filesystem::path session = must_find_session(kool_spin, logs);
    CHECK(std::filesystem::exists(session / "kool_spin.log"));
    CHECK(!std::filesystem::exists(state));
#endif
}

TEST_CASE("Logs: without a log folder", "[supervisor]") {
    LoggedKoolSpin kool_spin;
    const std::filesystem::path not_a_folder = kool_spin.dir() / "not-a-folder";
    write_file(not_a_folder, "a file\n");
    kool_spin.start(not_a_folder);
    Client client = must_join(kool_spin, "nofiles");
    const OpenedRing opened = must_open(client);
    CHECK(opened.slot == 0);

    CHECK(stderr_has(kool_spin.err(), "kool_spin: ", not_a_folder.string()));
    CHECK(stderr_has(kool_spin.err(), "ks-engine: ", "ready"));
    CHECK(stderr_has(kool_spin.err(), "kool_spin: ", "client 'nofiles' asked for a ring"));
}

TEST_CASE("Logs: standard error gets kool_spin's warnings and errors", "[supervisor]") {
    LoggedKoolSpin kool_spin;
    kool_spin.start();
    Client client = must_join(kool_spin, "greedy");
    const OpenedRing opened = must_open(client);
    const std::expected<OpenedRing, OpenFailure> second = open_ring(client.connection, 3);
    REQUIRE(!second.has_value());
    CHECK(second.error() == OpenFailure{ErrorCode::NotSupported});

    CHECK(stderr_has(kool_spin.err(), "kool_spin: ", "refused client 'greedy' a second ring"));
    // Give the engine's lines time to reach the supervisor, which reads them
    // at least every 100 ms.
    std::this_thread::sleep_for(Milliseconds{300});
    for (const std::string& line : read_lines(kool_spin.err())) {
        CAPTURE(line);
        CHECK(line.find("asked for a ring") == std::string::npos);
        CHECK(!line.starts_with("ks-engine: "));
    }
    CHECK(opened.slot == 0);
}
