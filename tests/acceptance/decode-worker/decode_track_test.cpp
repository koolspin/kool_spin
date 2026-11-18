// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/decode-worker/spec.md: decoding into a track
// segment (requirement 14).

#include <ks/decode/decoder.h>
#include <ks/ipc/track_segment.h>

#include "decode_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace {

using ks::decode::decode_track;
using ks::decode::DecodedTrack;
using ks::decode::Decoder;
using ks::decode::kDecodeChunkFrames;
using ks::decode::test::decode_all;
using ks::decode::test::FailingSource;
using ks::decode::test::fixture;
using ks::decode::test::frames_in;
using ks::decode::test::header_of;
using ks::decode::test::kSecond;
using ks::decode::test::make_track_segment;
using ks::decode::test::open_decoder;
using ks::decode::test::progress_of;
using ks::decode::test::samples_of;
using ks::decode::test::Segment;
using ks::decode::test::source_for;
using ks::decode::test::to_s16;
using ks::decode::test::tone;
using ks::decode::test::write_wav_s16;
using ks::decode::test::zero_after;
using ks::ipc::kMaxTrackFrames;
using ks::ipc::kTrackFrameSize;
using ks::ipc::kTrackSamplesOffset;
using ks::ipc::TrackProgress;
using ks::ipc::TrackStatus;
using ks::ipc::TrackWriter;
using ks::ipc::test::TempDir;

/// Never stops.
bool never() {
    return false;
}

/// Everything the file at `path` decodes to, from a decoder of its own.
std::vector<float> reference(const std::filesystem::path& path) {
    Decoder decoder = open_decoder(path);
    return decode_all(decoder);
}

/// True if the first `frames` frames of `segment` equal those of `expected`,
/// bit for bit.
bool frames_match(const Segment& segment, std::span<const float> expected, std::uint64_t frames) {
    const std::size_t count = static_cast<std::size_t>(frames) * 2;
    if (count > expected.size()) {
        return false;
    }
    const std::span<const float> samples = samples_of(segment);
    return std::equal(expected.begin(), expected.begin() + static_cast<std::ptrdiff_t>(count),
                      samples.begin());
}

} // namespace

TEST_CASE("Decode track: a whole file", "[decode-worker]") {
    const std::vector<float> expected = reference(fixture("tone.flac"));
    REQUIRE(frames_in(expected) == kSecond);

    const std::unique_ptr<Segment> segment = make_track_segment();
    TrackWriter writer{segment->region->bytes()};
    Decoder decoder = open_decoder(fixture("tone.flac"));

    // stopping() is asked before each read: what had been published by then.
    std::vector<TrackProgress> before_reads;
    int refused = 0;
    const DecodedTrack decoded = decode_track(decoder, writer, kMaxTrackFrames, [&] {
        const std::optional<TrackProgress> progress = progress_of(*segment);
        if (!progress) {
            ++refused;
        }
        before_reads.push_back(progress.value_or(TrackProgress{}));
        return false;
    });

    CHECK(decoded == DecodedTrack{.frames = kSecond, .status = TrackStatus::Complete});
    CHECK(progress_of(*segment) == TrackProgress{.sample_rate = 44'100,
                                                 .status = TrackStatus::Complete,
                                                 .frames = kSecond,
                                                 .complete = true});
    CHECK(header_of(*segment).sample_rate == 44'100);
    CHECK(frames_match(*segment, expected, kSecond));
    CHECK(zero_after(*segment, kSecond));

    // Started before the first read, and published after every read that
    // returned frames: at most kDecodeChunkFrames more each time.
    CHECK(refused == 0);
    REQUIRE(before_reads.size() >= kSecond / kDecodeChunkFrames);
    CHECK(before_reads.front().sample_rate == 44'100);
    CHECK(before_reads.front().frames == 0);
    for (std::size_t i = 1; i < before_reads.size(); ++i) {
        CAPTURE(i);
        CHECK(!before_reads[i].complete);
        CHECK(before_reads[i].frames > before_reads[i - 1].frames);
        CHECK(before_reads[i].frames - before_reads[i - 1].frames <= kDecodeChunkFrames);
    }
}

TEST_CASE("Decode track: the limit", "[decode-worker]") {
    const std::vector<float> expected = reference(fixture("tone.flac"));
    REQUIRE(frames_in(expected) == kSecond);

    SECTION("max_frames below the file's length") {
        const std::unique_ptr<Segment> segment = make_track_segment();
        TrackWriter writer{segment->region->bytes()};
        Decoder decoder = open_decoder(fixture("tone.flac"));
        CHECK(decode_track(decoder, writer, 10'000, never) ==
              DecodedTrack{.frames = 10'000, .status = TrackStatus::Truncated});
        CHECK(progress_of(*segment) == TrackProgress{.sample_rate = 44'100,
                                                     .status = TrackStatus::Truncated,
                                                     .frames = 10'000,
                                                     .complete = true});
        CHECK(frames_match(*segment, expected, 10'000));
        CHECK(zero_after(*segment, 10'000));
    }

    SECTION("max_frames exactly the file's length") {
        const std::unique_ptr<Segment> segment = make_track_segment();
        TrackWriter writer{segment->region->bytes()};
        Decoder decoder = open_decoder(fixture("tone.flac"));
        CHECK(decode_track(decoder, writer, kSecond, never) ==
              DecodedTrack{.frames = kSecond, .status = TrackStatus::Complete});
        CHECK(frames_match(*segment, expected, kSecond));
    }

    SECTION("the writer's capacity below max_frames") {
        // Room for 1,000 frames, rounded up to a page.
        const std::unique_ptr<Segment> segment =
            make_track_segment(kTrackSamplesOffset + (1'000 * kTrackFrameSize));
        TrackWriter writer{segment->region->bytes()};
        const std::uint64_t capacity = writer.capacity();
        REQUIRE(capacity >= 1'000);
        REQUIRE(capacity < kSecond);
        Decoder decoder = open_decoder(fixture("tone.flac"));
        CHECK(decode_track(decoder, writer, kMaxTrackFrames, never) ==
              DecodedTrack{.frames = capacity, .status = TrackStatus::Truncated});
        CHECK(frames_match(*segment, expected, capacity));
    }
}

TEST_CASE("Decode track: failures and stops", "[decode-worker]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());
    // 5 seconds, so FFmpeg has started decoding well before a failure half
    // way through.
    constexpr std::size_t kFrames = 5 * kSecond;
    const std::filesystem::path path = dir.path() / "tone.wav";
    write_wav_s16(path, 44'100, 2, to_s16(tone(44'100, 2, kFrames)));
    const std::vector<float> expected = reference(path);
    REQUIRE(frames_in(expected) == kFrames);

    SECTION("the source fails") {
        const auto size = static_cast<std::uint64_t>(std::filesystem::file_size(path));
        const std::unique_ptr<Segment> segment = make_track_segment();
        TrackWriter writer{segment->region->bytes()};
        Decoder decoder = open_decoder(std::make_unique<FailingSource>(source_for(path), size / 2));
        const DecodedTrack decoded = decode_track(decoder, writer, kMaxTrackFrames, never);
        CAPTURE(decoded.frames);
        CHECK(decoded.status == TrackStatus::Failed);
        CHECK(decoded.frames > 0);
        CHECK(decoded.frames < kFrames);
        CHECK(progress_of(*segment) == TrackProgress{.sample_rate = 44'100,
                                                     .status = TrackStatus::Failed,
                                                     .frames = decoded.frames,
                                                     .complete = true});
        CHECK(frames_match(*segment, expected, decoded.frames));
        CHECK(zero_after(*segment, decoded.frames));
    }

    SECTION("stopping after two reads") {
        const std::unique_ptr<Segment> segment = make_track_segment();
        TrackWriter writer{segment->region->bytes()};
        Decoder decoder = open_decoder(path);
        int calls = 0;
        const DecodedTrack decoded =
            decode_track(decoder, writer, kMaxTrackFrames, [&calls] { return ++calls >= 3; });
        CHECK(calls == 3);
        CHECK(decoded ==
              DecodedTrack{.frames = 2 * kDecodeChunkFrames, .status = TrackStatus::Failed});
        CHECK(progress_of(*segment) == TrackProgress{.sample_rate = 44'100,
                                                     .status = TrackStatus::Failed,
                                                     .frames = 2 * kDecodeChunkFrames,
                                                     .complete = true});
        CHECK(frames_match(*segment, expected, 2 * kDecodeChunkFrames));
        CHECK(zero_after(*segment, 2 * kDecodeChunkFrames));
    }

    SECTION("stopping before the first read") {
        const std::unique_ptr<Segment> segment = make_track_segment();
        TrackWriter writer{segment->region->bytes()};
        Decoder decoder = open_decoder(path);
        CHECK(decode_track(decoder, writer, kMaxTrackFrames, [] { return true; }) ==
              DecodedTrack{.frames = 0, .status = TrackStatus::Failed});
        // Started, so the rate is there.
        CHECK(progress_of(*segment) == TrackProgress{.sample_rate = 44'100,
                                                     .status = TrackStatus::Failed,
                                                     .frames = 0,
                                                     .complete = true});
        CHECK(zero_after(*segment, 0));
    }
}
