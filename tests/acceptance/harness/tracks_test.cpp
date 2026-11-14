// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/harness/spec.md: decoding files, and preparing a
// script's tracks (requirements 20 to 22).

#include <ks/harness/tracks.h>

#include <ks/engine/track.h>
#include <ks/harness/script.h>

#include "../deck/wav.h"
#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

using ks::engine::test::read_wav;
using ks::engine::test::WavAudio;
using ks::engine::test::write_wav;
using ks::harness::decode_file;
using ks::harness::DecodedAudio;
using ks::harness::read_script;
using ks::harness::Script;
using ks::harness::ScriptError;
using ks::harness::TrackSet;
using ks::harness::test::channel;
using ks::harness::test::data_dir;
using ks::harness::test::error_text;
using ks::harness::test::frequency;
using ks::harness::test::temp_dir;
using ks::harness::test::within;

using Decoded = std::expected<DecodedAudio, std::string>;

/// Decodes a fixture, which must work.
DecodedAudio decoded(const std::string& name) {
    const Decoded result = decode_file(data_dir() / name);
    CAPTURE(name, error_text(result));
    REQUIRE(result.has_value());
    return *result;
}

/// Whether two interleaved buffers match within `tolerance`.
bool matches(const std::vector<float>& a, const std::vector<float>& b, float tolerance) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i] - b[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

/// Checks a 1 kHz left and 2 kHz right test tone, away from its ends.
void check_tones(const DecodedAudio& audio, std::size_t first, std::size_t last) {
    const std::vector<float> left = channel(audio.samples, 2, 0);
    const std::vector<float> right = channel(audio.samples, 2, 1);
    const double left_hz = frequency(left, audio.sample_rate, first, last);
    const double right_hz = frequency(right, audio.sample_rate, first, last);
    CAPTURE(left_hz, right_hz);
    CHECK(within(left_hz, 1000.0, 0.01));
    CHECK(within(right_hz, 2000.0, 0.01));
}

} // namespace

TEST_CASE("Tracks: decoding files", "[harness]") {
    SECTION("16-bit mono WAV: both channels, at its own rate") {
        const DecodedAudio audio = decoded("mono_s16_22050.wav");
        CHECK(audio.sample_rate == 22'050);
        CHECK(audio.samples.size() == 2 * 5'513);
        CHECK(channel(audio.samples, 2, 0) == channel(audio.samples, 2, 1));
        const std::optional<WavAudio> reference = read_wav(data_dir() / "mono_s16_22050.wav");
        REQUIRE(reference.has_value());
        CHECK(matches(audio.samples, reference.value_or(WavAudio{}).samples, 1e-6F));
        CHECK(within(frequency(channel(audio.samples, 2, 0), 22'050, 0, 5'513), 1000.0, 0.01));
    }

    SECTION("float stereo WAV: exactly its samples") {
        const DecodedAudio audio = decoded("stereo_f32_48000.wav");
        CHECK(audio.sample_rate == 48'000);
        const std::optional<WavAudio> reference = read_wav(data_dir() / "stereo_f32_48000.wav");
        REQUIRE(reference.has_value());
        CHECK(audio.samples == reference.value_or(WavAudio{}).samples);
        check_tones(audio, 0, 12'000);
    }

    SECTION("FLAC") {
        const DecodedAudio audio = decoded("stereo_44100.flac");
        CHECK(audio.sample_rate == 44'100);
        CHECK(audio.samples.size() == 2 * 11'025);
        check_tones(audio, 0, 11'025);
    }

    SECTION("MP3") {
        const DecodedAudio audio = decoded("stereo_44100.mp3");
        CHECK(audio.sample_rate == 44'100);
        // An MP3 can gain or lose up to a frame or so of padding.
        const std::size_t frames = audio.samples.size() / 2;
        CAPTURE(frames);
        CHECK(frames >= 9'000);
        CHECK(frames <= 14'000);
        check_tones(audio, 2'000, 8'000);
    }

    SECTION("the lowest sample rate a deck accepts") {
        const std::filesystem::path dir = temp_dir("rate8000");
        const std::vector<float> silence(800, 0.0F);
        REQUIRE(write_wav(dir / "rate8000.wav", 8'000, silence, silence));
        const Decoded result = decode_file(dir / "rate8000.wav");
        CAPTURE(error_text(result));
        REQUIRE(result.has_value());
        CHECK(result->sample_rate == 8'000);
        std::filesystem::remove_all(dir);
    }

    SECTION("errors name the file") {
        const std::filesystem::path dir = temp_dir("errors");

        const Decoded missing = decode_file(dir / "nothere.wav");
        REQUIRE(!missing.has_value());
        CAPTURE(missing.error());
        CHECK(missing.error().find("nothere.wav") != std::string::npos);

        {
            std::ofstream text{dir / "not_audio.wav"};
            text << "This isn't audio.\n";
        }
        const Decoded not_audio = decode_file(dir / "not_audio.wav");
        REQUIRE(!not_audio.has_value());
        CAPTURE(not_audio.error());
        CHECK(not_audio.error().find("not_audio.wav") != std::string::npos);

        const std::vector<float> silence(400, 0.0F);
        REQUIRE(write_wav(dir / "rate4000.wav", 4'000, silence, silence));
        const Decoded slow = decode_file(dir / "rate4000.wav");
        REQUIRE(!slow.has_value());
        CAPTURE(slow.error());
        CHECK(slow.error().find("rate4000.wav") != std::string::npos);

        std::filesystem::remove_all(dir);
    }

    SECTION("preparing a script's tracks: complete, with IDs in script order") {
        const std::expected<Script, ScriptError> script =
            read_script("track a file \"stereo_f32_48000.wav\"\n"
                        "track b sine 440 0.5\n"
                        "0 load 1 a\n"
                        "1 end\n",
                        data_dir());
        CAPTURE(error_text(script));
        REQUIRE(script.has_value());
        const std::expected<TrackSet, std::string> tracks = TrackSet::prepare(*script);
        CAPTURE(error_text(tracks));
        REQUIRE(tracks.has_value());
        REQUIRE(tracks->size() == 2);

        const ks::engine::Track& file = tracks->track(0);
        CHECK(file.id() == 1);
        CHECK(file.sample_rate() == 48'000);
        CHECK(file.capacity() == 12'000);
        CHECK(file.ready().complete);
        CHECK(file.ready().frames == 12'000);

        const ks::engine::Track& sine = tracks->track(1);
        CHECK(sine.id() == 2);
        CHECK(sine.sample_rate() == 44'100);
        CHECK(sine.capacity() == 22'050);
        CHECK(sine.ready().complete);
        CHECK(sine.ready().frames == 22'050);
    }

    SECTION("preparing fails, naming the file, if one can't be decoded") {
        const std::expected<Script, ScriptError> script =
            read_script("track a file \"missing.flac\"\n0 load 1 a\n1 end\n", data_dir());
        CAPTURE(error_text(script));
        REQUIRE(script.has_value());
        const std::expected<TrackSet, std::string> tracks = TrackSet::prepare(*script);
        REQUIRE(!tracks.has_value());
        CAPTURE(tracks.error());
        CHECK(tracks.error().find("missing.flac") != std::string::npos);
    }
}
