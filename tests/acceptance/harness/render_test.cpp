// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/harness/spec.md: rendering a script into a file
// (requirements 17 to 20, 24, and 28).

#include <ks/harness/run.h>
#include <ks/harness/script.h>
#include <ks/harness/tracks.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using ks::harness::FileOptions;
using ks::harness::read_script;
using ks::harness::render_to_file;
using ks::harness::Report;
using ks::harness::Script;
using ks::harness::ScriptError;
using ks::harness::TrackSet;
using ks::harness::test::channel;
using ks::harness::test::dbfs;
using ks::harness::test::error_text;
using ks::harness::test::FloatWav;
using ks::harness::test::frequency;
using ks::harness::test::peak;
using ks::harness::test::read_bytes;
using ks::harness::test::read_float_wav;
using ks::harness::test::temp_dir;
using ks::harness::test::within;

/// A script and its tracks, ready to render.
struct Prepared {
    Script script;
    TrackSet tracks;
};

Prepared prepare(const std::string& text) {
    std::expected<Script, ScriptError> script = read_script(text, "/");
    CAPTURE(error_text(script));
    REQUIRE(script.has_value());
    std::expected<TrackSet, std::string> tracks = TrackSet::prepare(*script);
    CAPTURE(error_text(tracks));
    REQUIRE(tracks.has_value());
    return Prepared{.script = std::move(*script), .tracks = std::move(*tracks)};
}

/// Renders, which must work, and returns the report.
Report render(const Prepared& prepared, const FileOptions& options) {
    const std::atomic<bool> stop{false};
    const std::expected<Report, std::string> report =
        render_to_file(prepared.script, prepared.tracks, options, stop);
    CAPTURE(error_text(report));
    REQUIRE(report.has_value());
    return *report;
}

FloatWav read_output(const std::filesystem::path& path) {
    const std::optional<FloatWav> wav = read_float_wav(path);
    REQUIRE(wav.has_value());
    return wav.value_or(FloatWav{});
}

/// Whether every sample in [first, last) is exactly 0.
bool silent(std::span<const float> samples, std::size_t first, std::size_t last) {
    return peak(samples.subspan(first, last - first)) == 0.0;
}

/// Checks that deck 1's sine starts at frame `start` on channels 0 and 1,
/// with the deck's fade-in, and that nothing plays before it. The fade-in's
/// gain is 0 on the frame the deck starts and rises from the next (deck
/// spec), so frame `start` is the last silent one: an event a frame early
/// or late fails.
void check_starts_at(const FloatWav& wav, std::size_t start) {
    for (std::size_t c = 0; c < 2; ++c) {
        CAPTURE(c);
        const std::vector<float> samples = channel(wav.samples, wav.channels, c);
        CHECK(silent(samples, 0, start + 1));
        CHECK(samples.at(start + 1) != 0.0F);

        // The deck fades in over 5 ms: its first 1 ms is quieter than what
        // follows the fade.
        const std::size_t ms = wav.sample_rate / 1000;
        const std::size_t steady = start + (10 * ms);
        REQUIRE(steady < samples.size());
        const std::span<const float> after = std::span{samples}.subspan(steady);
        CHECK(peak(std::span{samples}.subspan(start, ms)) < peak(after) / 2.0);
        const double level = dbfs(peak(after));
        CAPTURE(level);
        CHECK(level >= -12.5);
        CHECK(level <= -11.5);
        CHECK(within(frequency(after, wav.sample_rate, 0, after.size()), 1000.0, 0.01));
    }
}

} // namespace

TEST_CASE("File output: events land on their frames", "[harness]") {
    const std::filesystem::path dir = temp_dir("frames");

    SECTION("48 kHz stereo, 256-frame blocks") {
        const Prepared prepared = prepare("track tone sine 1000 5 rate 48000\n"
                                          "0 load 1 tone\n"
                                          "0.5 play 1\n"
                                          "1 end\n");
        const FileOptions options{.path = dir / "out.wav"};
        const Report report = render(prepared, options);
        CHECK(report.played_seconds == 1.0);
        CHECK(!report.stopped_early);
        CHECK(report.commands_sent == 1);
        CHECK(report.commands_read == 1);
        CHECK(report.commands_rejected == 0);
        CHECK(report.events_delayed == 0);
        CHECK(report.device_losses == 0);

        const FloatWav wav = read_output(options.path);
        CHECK(wav.sample_rate == 48'000);
        CHECK(wav.channels == 2);
        CHECK(wav.frames() == 48'000);
        check_starts_at(wav, 24'000);
    }

    SECTION("44.1 kHz on 4 channels, 128-frame blocks, at a time between frames") {
        const Prepared prepared = prepare("track tone sine 1000 5 rate 48000\n"
                                          "0 load 1 tone\n"
                                          "0.1234 play 1\n" // frame 5441.94
                                          "0.5 end\n");
        const FileOptions options{
            .path = dir / "four.wav", .sample_rate = 44'100, .channels = 4, .block_frames = 128};
        const Report report = render(prepared, options);
        CHECK(report.played_seconds == 22'050.0 / 44'100.0);

        const FloatWav wav = read_output(options.path);
        CHECK(wav.sample_rate == 44'100);
        CHECK(wav.channels == 4);
        CHECK(wav.frames() == 22'050);
        check_starts_at(wav, 5'442);
        // Nothing is cued, so the headphone channels are silent.
        CHECK(silent(channel(wav.samples, 4, 2), 0, 22'050));
        CHECK(silent(channel(wav.samples, 4, 3), 0, 22'050));
    }

    std::filesystem::remove_all(dir);
}

TEST_CASE("File output: the same script renders the same file", "[harness]") {
    const std::filesystem::path dir = temp_dir("same");
    // No delays, and at most 32 commands at any one time.
    const Prepared prepared = prepare("track a sweep 100 8000 3\n"
                                      "track b noise 3 rate 48000\n"
                                      "track c clicks 140 3 rate 32000\n"
                                      "0 load 1 a\n"
                                      "0 load 2 b\n"
                                      "0 load 3 c\n"
                                      "0 trim 2 -12\n"
                                      "0 play 1\n"
                                      "0 play 2\n"
                                      "0 play 3\n"
                                      "0.25 rate 1 1.5\n"
                                      "0.5 seek 2 1.25\n"
                                      "0.75 crossfader 0.2\n"
                                      "0.75 assign 3 b\n"
                                      "1 rate 3 -0.5\n"
                                      "1.1 cuepoint 1 0.5\n"
                                      "1.25 pause 2\n"
                                      "1.4 fader 1 0.5\n"
                                      "1.5 end\n");

    const auto render_bytes = [&](const std::string& name, std::uint32_t block) {
        const FileOptions options{.path = dir / name, .block_frames = block};
        const Report report = render(prepared, options);
        CHECK(report.events_delayed == 0);
        return read_bytes(options.path);
    };
    const std::vector<char> first = render_bytes("first.wav", 256);
    REQUIRE(first.size() > std::size_t{72'000} * 2 * sizeof(float));
    CHECK(render_bytes("second.wav", 256) == first);
    CHECK(render_bytes("block1.wav", 1) == first);
    CHECK(render_bytes("block128.wav", 128) == first);

    std::filesystem::remove_all(dir);
}

TEST_CASE("File output: delayed commands", "[harness]") {
    const std::filesystem::path dir = temp_dir("delayed");
    // 300 commands at 0.1 s, more than the command ring holds: 299 close the
    // fader, and the last opens it.
    std::string text = "track tone sine 1000 2 rate 48000\n0 load 1 tone\n0 play 1\n";
    for (int i = 0; i < 299; ++i) {
        text += "0.1 fader 1 0\n";
    }
    text += "0.1 fader 1 1\n1 end\n";
    const Prepared prepared = prepare(text);

    const FileOptions options{.path = dir / "out.wav"};
    const Report report = render(prepared, options);
    CHECK(report.commands_sent == 301);
    CHECK(report.commands_read == 301);
    CHECK(report.commands_rejected == 0);
    CHECK(report.events_delayed > 0);

    const FloatWav wav = read_output(options.path);
    REQUIRE(wav.frames() == 48'000);
    const std::vector<float> left = channel(wav.samples, 2, 0);
    const std::span<const float> all{left};

    // The closed fader silenced a stretch after 0.1 s...
    bool silenced = false;
    for (std::size_t at = 4'800; at + 256 <= 9'600; at += 64) {
        silenced = silenced || peak(all.subspan(at, 256)) < std::pow(10.0, -60.0 / 20.0);
    }
    CHECK(silenced);
    // ...and the last command, read last, opened it again.
    const double level = dbfs(peak(all.subspan(43'200)));
    CAPTURE(level);
    CHECK(level >= -12.5);
    CHECK(level <= -11.5);

    std::filesystem::remove_all(dir);
}
