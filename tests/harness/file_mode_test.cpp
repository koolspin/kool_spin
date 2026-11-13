// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for file mode (specs/harness/spec.md requirements 24 and 26).

#include <ks/harness/run.h>
#include <ks/harness/script.h>
#include <ks/harness/tracks.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <utility>

namespace {

using ks::harness::FileOptions;
using ks::harness::read_script;
using ks::harness::render_to_file;
using ks::harness::Report;
using ks::harness::Script;
using ks::harness::ScriptError;
using ks::harness::TrackSet;

Script script_of(const std::string& text) {
    std::expected<Script, ScriptError> script = read_script(text, "/");
    REQUIRE(script.has_value());
    return std::move(*script);
}

TrackSet tracks_of(const Script& script) {
    std::expected<TrackSet, std::string> tracks = TrackSet::prepare(script);
    REQUIRE(tracks.has_value());
    return std::move(*tracks);
}

std::filesystem::path temp_path(const std::string& name) {
    return std::filesystem::temp_directory_path() / ("ks_harness_file_mode_" + name + ".wav");
}

const char* const kScript = "track t sine 1000 1 rate 48000\n"
                            "0 load 1 t\n"
                            "0.001 play 1\n"      // frame 48: inside the first block
                            "0.0105 rate 1 0.5\n" // frame 504
                            "0.02 end\n";         // frame 960

} // namespace

TEST_CASE("File mode: renders to the end, splitting blocks at events", "[harness]") {
    const Script script = script_of(kScript);
    const TrackSet tracks = tracks_of(script);
    const std::atomic<bool> stop{false};
    const FileOptions options{.path = temp_path("split"), .block_frames = 100};

    const std::expected<Report, std::string> report = render_to_file(script, tracks, options, stop);
    REQUIRE(report.has_value());
    CHECK(report->played_seconds == 960.0 / 48'000.0);
    CHECK(!report->stopped_early);
    // Blocks end at 48, 148, 248, 348, 448, 504, 604, 704, 804, 904, 960.
    CHECK(report->blocks_rendered == 11);
    CHECK(report->commands_sent == 2);
    CHECK(report->commands_read == 2);
    CHECK(report->events_delayed == 0);
    CHECK(std::filesystem::file_size(options.path) > std::uintmax_t{960} * 2 * sizeof(float));
    std::filesystem::remove(options.path);
}

TEST_CASE("File mode: the stop flag stops before the next block", "[harness]") {
    const Script script = script_of(kScript);
    const TrackSet tracks = tracks_of(script);
    const std::atomic<bool> stop{true};
    const FileOptions options{.path = temp_path("stop")};

    const std::expected<Report, std::string> report = render_to_file(script, tracks, options, stop);
    REQUIRE(report.has_value());
    CHECK(report->stopped_early);
    CHECK(report->played_seconds == 0.0);
    CHECK(report->blocks_rendered == 0);
    std::filesystem::remove(options.path);
}

TEST_CASE("File mode: a file that can't be written is an error naming it", "[harness]") {
    const Script script = script_of(kScript);
    const TrackSet tracks = tracks_of(script);
    const std::atomic<bool> stop{false};
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "ks_harness_no_such_dir" / "out.wav";
    const FileOptions options{.path = path};

    const std::expected<Report, std::string> report = render_to_file(script, tracks, options, stop);
    REQUIRE(!report.has_value());
    CHECK(report.error().find(path.string()) != std::string::npos);
}
