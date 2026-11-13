// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for decoding files and preparing a script's tracks
// (specs/harness/spec.md requirements 21 and 22).

#include <ks/harness/tracks.h>

#include <ks/engine/track.h>
#include <ks/harness/script.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using ks::harness::decode_file;
using ks::harness::DecodedAudio;
using ks::harness::generate;
using ks::harness::read_script;
using ks::harness::Script;
using ks::harness::ScriptError;
using ks::harness::Signal;
using ks::harness::TrackSet;

void put_u16(std::ofstream& out, std::uint16_t value) {
    const std::array<char, 2> bytes{static_cast<char>(value & 0xffU),
                                    static_cast<char>((value >> 8U) & 0xffU)};
    out.write(bytes.data(), bytes.size());
}

void put_u32(std::ofstream& out, std::uint32_t value) {
    put_u16(out, static_cast<std::uint16_t>(value & 0xffffU));
    put_u16(out, static_cast<std::uint16_t>(value >> 16U));
}

/// Writes a silent 16-bit mono PCM WAV file of `frames` frames at `rate`.
void write_silent_wav(const std::filesystem::path& path, std::uint32_t rate, std::uint32_t frames) {
    std::ofstream out{path, std::ios::binary};
    const std::uint32_t data_bytes = frames * 2U;
    out.write("RIFF", 4);
    put_u32(out, 36U + data_bytes);
    out.write("WAVEfmt ", 8);
    put_u32(out, 16);
    put_u16(out, 1); // PCM
    put_u16(out, 1); // mono
    put_u32(out, rate);
    put_u32(out, rate * 2U);
    put_u16(out, 2);
    put_u16(out, 16);
    out.write("data", 4);
    put_u32(out, data_bytes);
    for (std::uint32_t i = 0; i < frames; ++i) {
        put_u16(out, 0);
    }
}

std::filesystem::path scratch_dir(const std::string& name) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("ks_harness_tracks_test_" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

Script script_of(const char* text, const std::filesystem::path& directory = {}) {
    std::expected<Script, ScriptError> script = read_script(text, directory);
    REQUIRE(script.has_value());
    return std::move(*script);
}

} // namespace

TEST_CASE("decode_file: errors name the file", "[harness]") {
    const std::filesystem::path dir = scratch_dir("errors");

    const std::expected<DecodedAudio, std::string> missing = decode_file(dir / "absent.flac");
    REQUIRE(!missing.has_value());
    CHECK(missing.error().find("absent.flac") != std::string::npos);

    // One below the lowest rate. (miniaudio itself refuses to open a file
    // above 384 kHz, so the upper limit can't be reached through it.)
    write_silent_wav(dir / "slow.wav", ks::engine::kMinTrackRate - 1, 100);
    const std::expected<DecodedAudio, std::string> slow = decode_file(dir / "slow.wav");
    REQUIRE(!slow.has_value());
    CAPTURE(slow.error());
    CHECK(slow.error().find("slow.wav") != std::string::npos);
    CHECK(slow.error().find("7999 Hz") != std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("decode_file: the rate limits are inclusive, and mono fills both channels", "[harness]") {
    const std::filesystem::path dir = scratch_dir("limits");
    for (const std::uint32_t rate : {ks::engine::kMinTrackRate, ks::engine::kMaxTrackRate}) {
        write_silent_wav(dir / "edge.wav", rate, 10);
        const std::expected<DecodedAudio, std::string> audio = decode_file(dir / "edge.wav");
        REQUIRE(audio.has_value());
        CHECK(audio->sample_rate == rate);
        CHECK(audio->samples.size() == 20);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("TrackSet::prepare: signals in script order, complete", "[harness]") {
    const Script script = script_of("track one sine 440 0.25\n"
                                    "track two noise 0.1 rate 48000\n"
                                    "0 load 1 one\n"
                                    "1 end\n");
    const std::expected<TrackSet, std::string> prepared = TrackSet::prepare(script);
    REQUIRE(prepared.has_value());
    REQUIRE(prepared->size() == 2);

    for (std::size_t i = 0; i < 2; ++i) {
        const auto& signal = std::get<Signal>(script.tracks.at(i).source);
        const ks::engine::Track& track = prepared->track(i);
        const std::vector<float> expected = generate(signal);
        CHECK(track.id() == i + 1);
        CHECK(track.sample_rate() == signal.sample_rate);
        CHECK(track.capacity() == expected.size() / 2);
        CHECK(track.ready().complete);
        CHECK(track.ready().frames == track.capacity());
        const std::span<const float> samples = track.samples();
        CHECK(std::vector<float>(samples.begin(), samples.end()) == expected);
    }
}

TEST_CASE("TrackSet: moving keeps the tracks' memory in place", "[harness]") {
    const Script script = script_of("track a sine 1000 0.1\n0 load 1 a\n1 end\n");
    std::expected<TrackSet, std::string> prepared = TrackSet::prepare(script);
    REQUIRE(prepared.has_value());
    const float* before = prepared->track(0).samples().data();
    const TrackSet moved = std::move(*prepared);
    CHECK(moved.track(0).samples().data() == before);
    CHECK(moved.track(0).ready().complete);
}

TEST_CASE("TrackSet::prepare: fails naming a file that can't be decoded", "[harness]") {
    const std::filesystem::path dir = scratch_dir("prepare");
    write_silent_wav(dir / "ok.wav", 44'100, 100);
    const Script script = script_of("track a file \"ok.wav\"\n"
                                    "track b file \"broken.wav\"\n"
                                    "0 load 1 a\n"
                                    "1 end\n",
                                    dir);
    const std::expected<TrackSet, std::string> prepared = TrackSet::prepare(script);
    REQUIRE(!prepared.has_value());
    CHECK(prepared.error().find("broken.wav") != std::string::npos);
    std::filesystem::remove_all(dir);
}
