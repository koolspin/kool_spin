// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/decode-worker/spec.md: decoding with FFmpeg
// (requirements 9 to 13).

#include <ks/decode/decoder.h>

#include <ks/ipc/fd.h>

#include "decode_support.h"
#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ks::decode::DecodeError;
using ks::decode::Decoder;
using ks::decode::FdSource;
using ks::decode::Refusal;
using ks::decode::RefusalReason;
using ks::decode::test::channel_of;
using ks::decode::test::decode_all;
using ks::decode::test::FailingSource;
using ks::decode::test::fixture;
using ks::decode::test::frames_in;
using ks::decode::test::frequency_of;
using ks::decode::test::kSecond;
using ks::decode::test::kToneRmsDb;
using ks::decode::test::open_decoder;
using ks::decode::test::refusal_of;
using ks::decode::test::rms_db;
using ks::decode::test::source_for;
using ks::decode::test::to_s16;
using ks::decode::test::tone;
using ks::decode::test::write_changed_copy;
using ks::decode::test::write_long_claiming_flac;
using ks::decode::test::write_wav_f32;
using ks::decode::test::write_wav_s16;
using ks::ipc::test::TempDir;

/// Frames in the exact-samples WAV: below 65,536 / 7, so every frame differs.
constexpr std::size_t kExactFrames = 9'000;

/// The exact-samples WAV's left sample for `frame`.
std::int16_t exact_left(std::size_t frame) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>((frame * 7U) & 0xFFFFU));
}

/// The exact-samples WAV's right sample for `frame`.
std::int16_t exact_right(std::size_t frame) {
    return static_cast<std::int16_t>(
        static_cast<std::uint16_t>((40'000U - (frame * 5U)) & 0xFFFFU));
}

/// Checks that `samples` hold a 1 kHz tone at -6 dBFS in both channels over
/// frames [from, to), measured as requirement "Formats" says.
void check_tone(std::span<const float> samples, std::size_t from, std::size_t to,
                double sample_rate) {
    for (std::size_t channel = 0; channel < 2; ++channel) {
        CAPTURE(channel);
        const std::vector<float> signal = channel_of(samples, channel, from, to);
        CHECK(std::abs(frequency_of(signal, sample_rate) - 1'000.0) <= 0.5);
        CHECK(std::abs(rms_db(signal) - kToneRmsDb) <= 0.5);
    }
}

} // namespace

TEST_CASE("Decoder: 16-bit samples are exact", "[decode-worker]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());

    std::vector<std::int16_t> stereo;
    std::vector<std::int16_t> mono;
    for (std::size_t frame = 0; frame < kExactFrames; ++frame) {
        stereo.push_back(exact_left(frame));
        stereo.push_back(exact_right(frame));
        mono.push_back(exact_left(frame));
    }
    write_wav_s16(dir.path() / "stereo.wav", 44'100, 2, stereo);
    write_wav_s16(dir.path() / "mono.wav", 44'100, 1, mono);

    SECTION("stereo") {
        Decoder decoder = open_decoder(dir.path() / "stereo.wav");
        CHECK(decoder.info().sample_rate == 44'100);
        CHECK(decoder.info().channels == 2);
        const std::vector<float> samples = decode_all(decoder);
        REQUIRE(frames_in(samples) == kExactFrames);
        for (std::size_t frame = 0; frame < kExactFrames; ++frame) {
            CAPTURE(frame);
            REQUIRE(samples[frame * 2] == static_cast<float>(exact_left(frame)) / 32'768.0F);
            REQUIRE(samples[(frame * 2) + 1] == static_cast<float>(exact_right(frame)) / 32'768.0F);
        }
    }

    SECTION("mono goes to both channels unchanged") {
        Decoder decoder = open_decoder(dir.path() / "mono.wav");
        CHECK(decoder.info().channels == 1);
        const std::vector<float> samples = decode_all(decoder);
        REQUIRE(frames_in(samples) == kExactFrames);
        for (std::size_t frame = 0; frame < kExactFrames; ++frame) {
            CAPTURE(frame);
            const float expected = static_cast<float>(exact_left(frame)) / 32'768.0F;
            REQUIRE(samples[frame * 2] == expected);
            REQUIRE(samples[(frame * 2) + 1] == expected);
        }
    }
}

TEST_CASE("Decoder: every format", "[decode-worker]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());
    const std::vector<float> source = tone(44'100, 2, kSecond);
    write_wav_s16(dir.path() / "tone_s16.wav", 44'100, 2, to_s16(source));
    write_wav_f32(dir.path() / "tone_f32.wav", 44'100, 2, source);

    struct Format {
        std::filesystem::path path;
        std::uint32_t sample_rate;
        bool lossless;
    };
    const std::vector<Format> formats{
        {dir.path() / "tone_s16.wav", 44'100, true}, {dir.path() / "tone_f32.wav", 44'100, true},
        {fixture("tone.aiff"), 44'100, true},        {fixture("tone.flac"), 44'100, true},
        {fixture("tone_alac.m4a"), 44'100, true},    {fixture("tone.mp3"), 44'100, false},
        {fixture("tone_aac.m4a"), 44'100, false},    {fixture("tone.ogg"), 44'100, false},
        {fixture("tone.opus"), 48'000, false},
    };
    for (const Format& format : formats) {
        CAPTURE(format.path.filename().string());
        Decoder decoder = open_decoder(format.path);
        CHECK(decoder.info().sample_rate == format.sample_rate);
        CHECK(decoder.info().channels == 2);
        const std::vector<float> samples = decode_all(decoder);
        const std::size_t frames = frames_in(samples);
        CAPTURE(frames);
        if (format.lossless) {
            CHECK(frames == format.sample_rate);
        } else {
            CHECK(frames + 2'048 >= format.sample_rate);
            CHECK(frames <= format.sample_rate + 2'048);
        }
        // The middle half second.
        check_tone(samples, format.sample_rate / 4, (format.sample_rate * 3) / 4,
                   format.sample_rate);
    }
}

TEST_CASE("Decoder: the file's own rate", "[decode-worker]") {
    Decoder decoder = open_decoder(fixture("tone_96k.flac"));
    CHECK(decoder.info().sample_rate == 96'000);
    const std::vector<float> samples = decode_all(decoder);
    CHECK(frames_in(samples) == 96'000);
    check_tone(samples, 24'000, 72'000, 96'000.0);
}

TEST_CASE("Decoder: more than two channels", "[decode-worker]") {
    Decoder decoder = open_decoder(fixture("surround.flac"));
    CHECK(decoder.info().sample_rate == 44'100);
    CHECK(decoder.info().channels == 6);
    const std::vector<float> samples = decode_all(decoder);
    REQUIRE(frames_in(samples) == 3 * kSecond);

    // Each second, leaving out a tenth at each end.
    const auto second = [&](std::size_t index, std::size_t channel) {
        return channel_of(samples, channel, (index * kSecond) + (kSecond / 10),
                          ((index + 1) * kSecond) - (kSecond / 10));
    };

    // The front left: in the left channel only.
    CHECK(std::abs(frequency_of(second(0, 0), 44'100.0) - 1'000.0) <= 0.5);
    CHECK(rms_db(second(0, 0)) > -20.0);
    CHECK(rms_db(second(0, 1)) < -80.0);

    // The front right: in the right channel only.
    CHECK(rms_db(second(1, 0)) < -80.0);
    CHECK(std::abs(frequency_of(second(1, 1), 44'100.0) - 1'000.0) <= 0.5);
    CHECK(rms_db(second(1, 1)) > -20.0);

    // The centre: in both, equal sample for sample.
    const std::vector<float> left = second(2, 0);
    CHECK(std::abs(frequency_of(left, 44'100.0) - 1'000.0) <= 0.5);
    CHECK(rms_db(left) > -20.0);
    CHECK(left == second(2, 1));
}

TEST_CASE("Decoder: a file with a picture", "[decode-worker]") {
    Decoder plain = open_decoder(fixture("tone.mp3"));
    Decoder pictured = open_decoder(fixture("picture.mp3"));
    CHECK(pictured.info().sample_rate == plain.info().sample_rate);
    CHECK(pictured.info().channels == plain.info().channels);
    CHECK(pictured.info().codec == plain.info().codec);
    const std::vector<float> expected = decode_all(plain);
    REQUIRE(!expected.empty());
    CHECK(decode_all(pictured) == expected);
}

TEST_CASE("Decoder: refused files", "[decode-worker]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());

    SECTION("a text file") {
        std::ofstream{dir.path() / "notes.txt"}
            << "These are not the samples you're looking for.\n";
        CHECK(refusal_of(dir.path() / "notes.txt") == RefusalReason::Unreadable);
    }

    SECTION("an empty file") {
        std::ofstream{dir.path() / "empty.wav"}.flush();
        CHECK(refusal_of(dir.path() / "empty.wav") == RefusalReason::Unreadable);
    }

    SECTION("a pipe") {
        std::array<int, 2> ends{-1, -1};
        REQUIRE(::pipe(ends.data()) == 0);
        ks::ipc::Fd read_end{ends[0]};
        const ks::ipc::Fd write_end{ends[1]};
        // Something to read, so only its not being a file can refuse it.
        constexpr std::string_view kStart = "RIFF";
        REQUIRE(::write(write_end.get(), kStart.data(), kStart.size()) ==
                static_cast<ssize_t>(kStart.size()));
        const std::expected<Decoder, Refusal> decoder =
            Decoder::open(std::make_unique<FdSource>(std::move(read_end)));
        REQUIRE_FALSE(decoder.has_value());
        CHECK(decoder.error().reason == RefusalReason::Unreadable);
    }

    SECTION("a rate below 8 kHz") {
        const std::vector<float> samples = tone(4'000, 2, 4'000);
        write_wav_s16(dir.path() / "4k.wav", 4'000, 2, to_s16(samples));
        CHECK(refusal_of(dir.path() / "4k.wav") == RefusalReason::BadRate);
    }

    SECTION("longer than the cap") {
        // 2^30 frames at 44.1 kHz: 6 hours, 45 minutes, and 47.9 seconds.
        write_long_claiming_flac(dir.path() / "long.flac", std::uint64_t{1} << 30U);
        const std::expected<Decoder, Refusal> decoder =
            Decoder::open(source_for(dir.path() / "long.flac"));
        REQUIRE_FALSE(decoder.has_value());
        CHECK(decoder.error().reason == RefusalReason::TooLong);
        CAPTURE(decoder.error().text);
        CHECK(decoder.error().text.find("405:48") != std::string::npos);
    }
}

TEST_CASE("Decoder: the estimated length", "[decode-worker]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());
    write_wav_s16(dir.path() / "tone.wav", 44'100, 2, to_s16(tone(44'100, 2, kSecond)));

    const Decoder flac = open_decoder(fixture("tone.flac"));
    CHECK(flac.info().estimated_frames == kSecond);
    CHECK(flac.info().codec == "flac");

    const Decoder wav = open_decoder(dir.path() / "tone.wav");
    CHECK(wav.info().estimated_frames == kSecond);
    CHECK(wav.info().codec == "pcm_s16le");
}

TEST_CASE("Decoder: the frames don't depend on read sizes", "[decode-worker]") {
    for (const std::string_view name : {"tone.mp3", "surround.flac"}) {
        CAPTURE(name);
        Decoder whole = open_decoder(fixture(name));
        const std::vector<float> expected = decode_all(whole);
        REQUIRE(!expected.empty());
        for (const std::size_t chunk : {std::size_t{1}, std::size_t{37}, std::size_t{1'000}}) {
            CAPTURE(chunk);
            Decoder decoder = open_decoder(fixture(name));
            CHECK(decode_all(decoder, chunk) == expected);
        }
    }
}

TEST_CASE("Decoder: damage in the middle", "[decode-worker]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());
    const std::filesystem::path damaged = dir.path() / "damaged.mp3";
    write_changed_copy("tone.mp3", damaged, [](std::vector<char>& bytes) {
        constexpr std::size_t kDamage = 2'048;
        REQUIRE(bytes.size() > 4 * kDamage);
        // std::mt19937's output is the same everywhere.
        std::mt19937 random{42};
        const std::size_t from = (bytes.size() - kDamage) / 2;
        for (std::size_t i = from; i < from + kDamage; ++i) {
            bytes[i] = static_cast<char>(random() & 0xFFU);
        }
    });

    Decoder decoder = open_decoder(damaged);
    const std::vector<float> samples = decode_all(decoder);
    const std::size_t frames = frames_in(samples);
    CAPTURE(frames, decoder.skipped_packets());
    CHECK(frames >= (kSecond * 8) / 10);
    REQUIRE(frames >= kSecond / 2);
    // The tone, in the first and last 0.3 s, leaving out the first and last
    // 0.05 s.
    check_tone(samples, kSecond / 20, (kSecond * 3) / 10, 44'100.0);
    check_tone(samples, frames - ((kSecond * 3) / 10), frames - (kSecond / 20), 44'100.0);
}

TEST_CASE("Decoder: the source fails", "[decode-worker]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());
    // 5 seconds, so FFmpeg has started decoding well before the failure.
    constexpr std::size_t kFrames = 5 * kSecond;
    const std::filesystem::path path = dir.path() / "tone.wav";
    write_wav_s16(path, 44'100, 2, to_s16(tone(44'100, 2, kFrames)));

    Decoder intact = open_decoder(path);
    const std::vector<float> expected = decode_all(intact);
    REQUIRE(frames_in(expected) == kFrames);

    const auto size = static_cast<std::uint64_t>(std::filesystem::file_size(path));
    Decoder decoder = open_decoder(std::make_unique<FailingSource>(source_for(path), size / 2));
    std::vector<float> samples;
    std::vector<float> out(ks::decode::kDecodeChunkFrames * 2);
    std::optional<DecodeError> error;
    for (std::size_t reads = 0; reads < 1'000 && !error; ++reads) {
        const std::expected<std::size_t, DecodeError> frames = decoder.read(out);
        if (!frames) {
            error = frames.error();
            break;
        }
        REQUIRE(*frames > 0);
        samples.insert(samples.end(), out.begin(),
                       out.begin() + static_cast<std::ptrdiff_t>(*frames * 2));
    }
    REQUIRE(error.has_value());
    const DecodeError failed = error.value_or(DecodeError{});
    CAPTURE(failed.text);
    CHECK(!failed.text.empty());
    // Some frames, every one of them good.
    CHECK(frames_in(samples) > 0);
    CHECK(frames_in(samples) < kFrames);
    REQUIRE(samples.size() <= expected.size());
    CHECK(std::equal(samples.begin(), samples.end(), expected.begin()));
    // The same error again.
    const std::expected<std::size_t, DecodeError> again = decoder.read(out);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == failed);
}
