// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/ks-play/spec.md: the ks-play program
// (requirements 21 to 30), run against kool_spin on the null backend.

#include <ks/client/session.h>
#include <ks/decode/decoder.h>

#include "decode_support.h"
#include "play_support.h"
#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace client = ks::client;
using ks::ks_play::test::find_label;
using ks::ks_play::test::Finished;
using ks::ks_play::test::lines_of;
using ks::ks_play::test::PlayRun;
using ks::ks_play::test::run_play;
using ks::ks_play::test::run_play_raw;
using ks::ks_play::test::status_field;
using ks::ks_play::test::status_lines;
using ks::ks_play::test::StatusLine;
using ks::track_loading::test::eventually;
using ks::track_loading::test::join;
using ks::track_loading::test::kFakeWorkerPath;
using ks::track_loading::test::Milliseconds;
using ks::track_loading::test::send_load;
using ks::track_loading::test::Session;
using ks::track_loading::test::write_fake;

std::filesystem::path flac() {
    return ks::decode::test::fixture("tone.flac");
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.starts_with(prefix);
}

/// True if `text` is exactly the line `expected`.
bool is_line(const std::string& text, const std::string& expected) {
    return text == expected + "\n";
}

} // namespace

TEST_CASE("ks-play: load, play, pause", "[play]") {
    const Session session;
    const std::filesystem::path& dir = session.dir();

    const Finished loaded = run_play(dir, {"load", "1", flac().string()});
    CHECK(loaded.status == 0);
    CHECK(std::regex_match(
        loaded.out,
        std::regex{R"(loaded track [0-9]+ on deck 1: 44100 Hz, estimated 44100 frames\n)"}));
    CHECK(loaded.err.empty());

    // A relative path gives the same result.
    const std::filesystem::path folder = flac().parent_path();
    const Finished relative = run_play(dir, {"load", "2", flac().filename().string()}, folder);
    CHECK(relative.status == 0);
    CHECK(std::regex_match(
        relative.out,
        std::regex{R"(loaded track [0-9]+ on deck 2: 44100 Hz, estimated 44100 frames\n)"}));

    const Finished play = run_play(dir, {"play", "1"});
    CHECK(play.status == 0);
    CHECK(is_line(play.out, "ok"));
    CHECK(play.err.empty());
    CHECK(eventually([&] {
        const auto before = status_field(dir, "deck 1", "position");
        std::this_thread::sleep_for(Milliseconds{200});
        const auto after = status_field(dir, "deck 1", "position");
        return before && after && std::stod(*after) > std::stod(*before) &&
               status_field(dir, "deck 1", "playing") == "1";
    }));

    CHECK(is_line(run_play(dir, {"pause", "1"}).out, "ok"));
    CHECK(eventually([&] { return status_field(dir, "deck 1", "playing") == "0"; }));

    SECTION("--wait") {
        const Finished waited = run_play(dir, {"load", "0", flac().string(), "--wait"});
        CHECK(waited.status == 0);
        const std::vector<std::string> lines = lines_of(waited.out);
        REQUIRE(lines.size() == 2);
        CHECK(starts_with(lines[0], "loaded track "));
        CHECK(lines[1] == "decoded 44100 frames: complete");
    }
}

TEST_CASE("ks-play: --wait reports a failed decode", "[play]") {
    const Session session{kFakeWorkerPath};
    const std::filesystem::path fake = write_fake(session.dir(), "failing",
                                                  {.estimate = 20'480,
                                                   .frames = 20'480,
                                                   .chunk_frames = 4'096,
                                                   .chunk_ms = 20,
                                                   .end = "failed"});
    const Finished waited = run_play(session.dir(), {"load", "0", fake.string(), "--wait"});
    CHECK(waited.status == 1);
    const std::vector<std::string> lines = lines_of(waited.out);
    REQUIRE(lines.size() == 2);
    CHECK(lines[1] == "decoded 20480 frames: failed");
}

TEST_CASE("ks-play: every control command", "[play]") {
    const Session session;
    const std::filesystem::path& dir = session.dir();
    REQUIRE(run_play(dir, {"load", "0", flac().string()}).status == 0);

    struct Case {
        std::vector<std::string> args;
        const char* label;
        const char* field;
        const char* value;
    };
    const std::vector<Case> cases{
        {{"rate", "0", "1.02"}, "deck 0", "rate", "1.02"},
        {{"seek", "0", "22050"}, "deck 0", "position", "22050"},
        {{"cue-point", "0", "11025"}, "deck 0", "cue", "11025"},
        {{"trim", "0", "-3"}, "channel 0", "trim_db", "-3"},
        {{"fader", "0", "0.5"}, "channel 0", "fader", "0.5"},
        {{"assign", "0", "b"}, "channel 0", "side", "b"},
        {{"channel-cue", "0", "on"}, "channel 0", "cue", "1"},
        {{"crossfader", "0.25"}, "mixer", "crossfader", "0.25"},
        {{"curve", "0.5"}, "mixer", "curve", "0.5"},
        {{"main-level", "-6"}, "mixer", "main_db", "-6"},
        {{"headphone-level", "-9"}, "mixer", "headphone_db", "-9"},
        {{"cue-mix", "0.75"}, "mixer", "cue_mix", "0.75"},
        {{"bus-map", "2", "4"}, "mixer", "bus_main", "2"},
        {{"bus-map", "2", "4"}, "mixer", "bus_cue", "4"},
        {{"trim", "0", "-inf"}, "channel 0", "trim_db", "-inf"},
        {{"main-level", "-inf"}, "mixer", "main_db", "-inf"},
        {{"headphone-level", "-inf"}, "mixer", "headphone_db", "-inf"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.args.front(), c.field, c.value);
        const Finished done = run_play(dir, c.args);
        CHECK(done.status == 0);
        CHECK(is_line(done.out, "ok"));
        CHECK(done.err.empty());
        CHECK(eventually([&] { return status_field(dir, c.label, c.field) == c.value; }));
    }
}

TEST_CASE("ks-play: rejections", "[play]") {
    const Session session;
    const std::filesystem::path& dir = session.dir();

    const Finished nan = run_play(dir, {"rate", "0", "nan"});
    CHECK(nan.status == 2);

    // The engine rejects odd bus channels.
    const Finished rejected = run_play(dir, {"bus-map", "1", "2"});
    CHECK(rejected.status == 1);
    CHECK(rejected.out.empty());
    CHECK(starts_with(rejected.err, "ks-play: "));
    CHECK(rejected.err.find("bad value") != std::string::npos);

    // The engine clamps out-of-range values instead of rejecting them, and a
    // deck with no track takes commands too.
    const Finished clamped = run_play(dir, {"rate", "0", "-1"});
    CHECK(clamped.status == 0);
    CHECK(is_line(clamped.out, "ok"));
    CHECK(is_line(run_play(dir, {"play", "3"}).out, "ok"));
}

TEST_CASE("ks-play: unload, ping, stop", "[play]") {
    Session session;
    const std::filesystem::path& dir = session.dir();
    REQUIRE(run_play(dir, {"load", "1", flac().string()}).status == 0);
    CHECK(eventually([&] { return status_field(dir, "deck 1", "loaded") == "1"; }));

    const Finished unloaded = run_play(dir, {"unload", "1"});
    CHECK(unloaded.status == 0);
    CHECK(is_line(unloaded.out, "unloaded deck 1"));
    CHECK(eventually([&] { return status_field(dir, "deck 1", "loaded") == "0"; }));

    const Finished ping = run_play(dir, {"ping"});
    CHECK(ping.status == 0);
    CHECK(is_line(ping.out, "pong"));

    const Finished stopped = run_play(dir, {"stop"});
    CHECK(stopped.status == 0);
    CHECK(is_line(stopped.out, "stopped"));
    CHECK(session.process().wait(Milliseconds{5'000}) == 0);
}

TEST_CASE("ks-play: status", "[play]") {
    const Session session;
    const std::filesystem::path& dir = session.dir();

    // A ring of the test's own, so one client slot is attached.
    client::Result<client::Session> own = client::Session::connect("holder", dir);
    REQUIRE(own.has_value());
    REQUIRE(own->open_ring().has_value());

    const Finished first = run_play(dir, {"status"});
    CHECK(first.status == 0);
    CHECK(first.err.empty());
    const std::vector<StatusLine> lines = status_lines(first.out);
    REQUIRE(lines.size() == 11);
    const std::vector<std::string> labels{"engine",    "deck 0",    "deck 1",    "deck 2",
                                          "deck 3",    "channel 0", "channel 1", "channel 2",
                                          "channel 3", "mixer",     "client 0"};
    const std::vector<std::vector<std::string>> fields{
        {"heartbeat", "state", "sample_rate", "muted", "silenced", "late_blocks",
         "non_finite_samples", "limited_frames", "render_load", "peak_render_load"},
        {"loaded", "playing", "waiting", "complete", "track", "track_rate", "track_frames",
         "frames_ready", "position", "rate", "cue"},
        {"trim_db", "fader", "side", "cue"},
        {"crossfader", "curve", "main_db", "headphone_db", "cue_mix", "bus_main", "bus_cue"},
        {"corrupt", "commands_read", "last_sequence", "rejected", "last_rejection"}};
    const auto fields_for = [&](std::size_t i) -> const std::vector<std::string>& {
        if (i == 0) {
            return fields[0];
        }
        if (i <= 4) {
            return fields[1];
        }
        if (i <= 8) {
            return fields[2];
        }
        return i == 9 ? fields[3] : fields[4];
    };
    for (std::size_t i = 0; i < lines.size(); ++i) {
        CAPTURE(i, labels[i]);
        CHECK(lines[i].label == labels[i]);
        std::vector<std::string> names;
        for (const auto& field : lines[i].fields) {
            names.push_back(field.first);
        }
        CHECK(names == fields_for(i));
    }
    CHECK(lines[0].get("state") == "playing");
    for (std::size_t i = 5; i <= 8; ++i) {
        const std::optional<std::string> side = lines[i].get("side");
        CHECK((side == "a" || side == "b" || side == "thru"));
    }

    const std::uint64_t beat = std::stoull(lines[0].get("heartbeat").value_or("0"));
    std::this_thread::sleep_for(Milliseconds{100});
    const Finished second = run_play(dir, {"status"});
    const std::optional<StatusLine> engine = find_label(status_lines(second.out), "engine");
    REQUIRE(engine.has_value());
    CHECK(std::stoull(engine.value_or(StatusLine{}).get("heartbeat").value_or("0")) > beat);
}

TEST_CASE("ks-play: watching", "[play]") {
    Session session;
    const std::filesystem::path& dir = session.dir();

    SECTION("snapshots, and SIGINT") {
        PlayRun run{
            dir, "/", {"--runtime-dir", dir.string(), "status", "--watch", "--interval", "100"}};
        std::this_thread::sleep_for(Milliseconds{1'200});
        run.signal(SIGINT);
        CHECK(run.wait(Milliseconds{1'000}) == 0);
        const std::string out = run.out();
        const std::vector<StatusLine> lines = status_lines(out);
        const auto engines = std::ranges::count_if(
            lines, [](const StatusLine& line) { return line.label == "engine"; });
        CHECK(engines >= 5);
        const std::vector<std::string> all = lines_of(out);
        CHECK(std::ranges::count(all, "---") == engines - 1);
        std::uint64_t last = 0;
        for (const StatusLine& line : lines) {
            if (line.label == "engine") {
                const std::uint64_t beat = std::stoull(line.get("heartbeat").value_or("0"));
                CHECK(beat >= last);
                last = beat;
            }
        }
    }

    SECTION("kool_spin killed") {
        PlayRun run{
            dir, "/", {"--runtime-dir", dir.string(), "status", "--watch", "--interval", "100"}};
        std::this_thread::sleep_for(Milliseconds{500});
        session.process().signal(SIGKILL);
        CHECK(run.wait(Milliseconds{5'000}) == 1);
        CHECK(starts_with(run.err(), "ks-play: "));
    }
}

TEST_CASE("ks-play: no kool_spin", "[play]") {
    const ks::ipc::test::TempDir empty;
    REQUIRE(!empty.path().empty());

    SECTION("nothing there") {
        const auto start = std::chrono::steady_clock::now();
        const Finished done = run_play(empty.path(), {"ping"});
        CHECK(done.status == 3);
        CHECK(starts_with(done.err, "ks-play: "));
        CHECK(done.out.empty());
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
    }

    SECTION("a stale socket file") {
        const std::filesystem::path path = empty.path() / "supervisor.sock";
        REQUIRE(ks::ipc::test::leave_socket_file(path));
        REQUIRE(std::filesystem::exists(path));
        const auto start = std::chrono::steady_clock::now();
        const Finished done = run_play(empty.path(), {"status"});
        CHECK(done.status == 3);
        CHECK(starts_with(done.err, "ks-play: "));
        CHECK(done.out.empty());
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
    }
}

TEST_CASE("ks-play: errors from kool_spin", "[play]") {
    const Session session{kFakeWorkerPath};
    const std::filesystem::path& dir = session.dir();

    const Finished missing = run_play(dir, {"load", "0", "/nonexistent/file.flac"});
    CHECK(missing.status == 1);
    CHECK(starts_with(missing.err, "ks-play: "));
    CHECK(missing.err.find("refused") != std::string::npos);

    // Four workers that never answer fill every place.
    ks::track_loading::test::TrackClient holder = join(session, "holder");
    for (int i = 0; i < 4; ++i) {
        static_cast<void>(
            send_load(holder, static_cast<std::uint32_t>(i),
                      write_fake(dir, "hang" + std::to_string(i), {.probe_hang = true})));
    }
    std::this_thread::sleep_for(Milliseconds{500});
    const Finished fifth = run_play(
        dir, {"load", "0", write_fake(dir, "fifth", {.estimate = 10, .frames = 10}).string()});
    CHECK(fifth.status == 1);
    CHECK(fifth.err.find("unavailable") != std::string::npos);
}

TEST_CASE("ks-play: usage", "[play]") {
    const ks::ipc::test::TempDir scratch;
    REQUIRE(!scratch.path().empty());

    const std::vector<std::vector<std::string>> bad{
        {},
        {"frobnicate"},
        // Missing, and extra, arguments.
        {"load", "0"},
        {"load"},
        {"load", "0", "/a.flac", "extra"},
        {"unload"},
        {"unload", "0", "1"},
        {"play"},
        {"play", "0", "1"},
        {"pause"},
        {"rate", "0"},
        {"rate", "0", "1", "2"},
        {"seek", "0"},
        {"cue-point", "0"},
        {"trim", "0"},
        {"fader", "0"},
        {"assign", "0"},
        {"assign", "0", "a", "b"},
        {"channel-cue", "0"},
        {"crossfader"},
        {"crossfader", "0.5", "1"},
        {"curve"},
        {"main-level"},
        {"headphone-level"},
        {"cue-mix"},
        {"bus-map", "2"},
        {"bus-map", "2", "4", "6"},
        {"status", "extra"},
        {"status", "--interval"},
        {"stop", "now"},
        {"ping", "now"},
        // Decks, and numbers.
        {"play", "4"},
        {"play", "-1"},
        {"play", "1.5"},
        {"play", " 1"},
        {"play", ""},
        {"rate", "0", "1x"},
        {"rate", "0", "0x10"},
        {"rate", "0", "inf"},
        {"rate", "0", "nan"},
        {"rate", "0", "-inf"},
        {"fader", "0", "-inf"},
        {"main-level", "inf"},
        {"bus-map", "2.5", "4"},
        // Paths, words, and options.
        {"load", "0", ""},
        {"assign", "0", "c"},
        {"channel-cue", "0", "maybe"},
        {"status", "--interval", "49"},
        {"status", "--interval", "60001"},
        {"status", "--interval", "x"},
        {"--runtime-dir", "/a", "--runtime-dir", "/b", "ping"},
        {"--name", std::string(65, 'n'), "ping"},
        {"--name", "", "ping"},
        {"--unknown", "ping"},
    };
    for (const std::vector<std::string>& args : bad) {
        CAPTURE(args);
        const Finished done = run_play_raw(scratch.path(), args);
        CHECK(done.status == 2);
        CHECK(starts_with(done.err, "ks-play: "));
        CHECK(done.out.empty());
    }

    const Finished help = run_play_raw(scratch.path(), {"--help"});
    CHECK(help.status == 0);
    CHECK_FALSE(help.out.empty());
}

TEST_CASE("ks-play: interrupted", "[play]") {
    const Session session{kFakeWorkerPath};
    const std::filesystem::path& dir = session.dir();
    const std::filesystem::path slow =
        write_fake(dir, "slow", {.estimate = 100, .probe_delay_ms = 30'000, .frames = 100});

    PlayRun run{dir, "/", {"--runtime-dir", dir.string(), "load", "0", slow.string()}};
    std::this_thread::sleep_for(Milliseconds{500});
    REQUIRE(run.running());
    run.signal(SIGTERM);
    CHECK(run.wait(Milliseconds{1'000}) == 143);

    // kool_spin goes on, and serves the next client.
    const Finished ping = run_play(dir, {"ping"});
    CHECK(ping.status == 0);
    CHECK(is_line(ping.out, "pong"));
}
