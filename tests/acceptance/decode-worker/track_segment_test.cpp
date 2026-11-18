// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/decode-worker/spec.md: the track segment's
// layout, reading its header, and writing it (requirements 3 to 7).

#include <ks/ipc/track_segment.h>

#include <ks/engine/track.h>
#include <ks/ipc/region.h>

#include "decode_support.h"
#include "track_checks.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>

namespace {

using ks::decode::test::capacity_of;
using ks::decode::test::check_track_input;
using ks::decode::test::header_of;
using ks::decode::test::kTrackInputSize;
using ks::decode::test::make_track_segment;
using ks::decode::test::progress_of;
using ks::decode::test::Segment;
using ks::ipc::kMaxTrackFrames;
using ks::ipc::kMaxTrackRate;
using ks::ipc::kMinTrackRate;
using ks::ipc::kTrackComplete;
using ks::ipc::kTrackSamplesOffset;
using ks::ipc::kTrackSegmentSize;
using ks::ipc::read_track;
using ks::ipc::TrackHeader;
using ks::ipc::TrackHeaderError;
using ks::ipc::TrackProgress;
using ks::ipc::TrackStatus;
using ks::ipc::TrackWriter;

/// The capacity the reading tests give read_track().
constexpr std::uint64_t kCapacity = 1'000;

/// A header with these fields.
TrackHeader header(std::uint64_t sample_rate, TrackStatus status, std::uint64_t ready) {
    return TrackHeader{
        .sample_rate = sample_rate, .status = static_cast<std::uint64_t>(status), .ready = ready};
}

/// What read_track() returns for `header`, or nothing if it refused it.
std::optional<TrackProgress> read(const TrackHeader& header, std::uint64_t capacity = kCapacity) {
    const std::expected<TrackProgress, TrackHeaderError> progress = read_track(header, capacity);
    if (!progress) {
        return std::nullopt;
    }
    return *progress;
}

/// Why read_track() refused `header`, or nothing if it didn't.
std::optional<TrackHeaderError> refusal(const TrackHeader& header) {
    const std::expected<TrackProgress, TrackHeaderError> progress = read_track(header, kCapacity);
    if (progress) {
        return std::nullopt;
    }
    return progress.error();
}

/// The fuzz target's input for `header` and `capacity`.
std::array<std::uint8_t, kTrackInputSize> input_for(const TrackHeader& header,
                                                    std::uint64_t capacity) {
    std::array<std::uint8_t, kTrackInputSize> input{};
    std::memcpy(input.data(), &header, sizeof header);
    std::memcpy(std::span{input}.subspan(sizeof header).data(), &capacity, sizeof capacity);
    return input;
}

} // namespace

TEST_CASE("Track segment: the layout", "[decode-worker]") {
    STATIC_CHECK(offsetof(TrackHeader, sample_rate) == 0);
    STATIC_CHECK(offsetof(TrackHeader, status) == 8);
    STATIC_CHECK(offsetof(TrackHeader, ready) == 16);
    STATIC_CHECK(std::is_standard_layout_v<TrackHeader>);
    STATIC_CHECK(std::is_trivially_copyable_v<TrackHeader>);

    STATIC_CHECK(ks::ipc::kTrackLayoutVersion == 1);
    STATIC_CHECK(kMinTrackRate == 8'000);
    STATIC_CHECK(kMaxTrackRate == 384'000);
    STATIC_CHECK(kMaxTrackFrames == std::uint64_t{1} << 28U);
    STATIC_CHECK(kTrackSamplesOffset == 16'384);
    STATIC_CHECK(kTrackSegmentSize == kTrackSamplesOffset + (std::size_t{1} << 31U));
    STATIC_CHECK(kTrackComplete == std::uint64_t{1} << 63U);

    STATIC_CHECK(static_cast<int>(TrackStatus::Decoding) == 0);
    STATIC_CHECK(static_cast<int>(TrackStatus::Complete) == 1);
    STATIC_CHECK(static_cast<int>(TrackStatus::Truncated) == 2);
    STATIC_CHECK(static_cast<int>(TrackStatus::Failed) == 3);

    // The engine's limits are the segment's.
    STATIC_CHECK(ks::engine::kMinTrackRate == kMinTrackRate);
    STATIC_CHECK(ks::engine::kMaxTrackRate == kMaxTrackRate);
    STATIC_CHECK(ks::engine::kMaxTrackFrames == kMaxTrackFrames);
}

TEST_CASE("Track segment: reading the header", "[decode-worker]") {
    SECTION("valid headers") {
        // Zero-filled: not started.
        CHECK(read(TrackHeader{}) == TrackProgress{.sample_rate = 0,
                                                   .status = TrackStatus::Decoding,
                                                   .frames = 0,
                                                   .complete = false});
        // Not started, whatever its rate.
        CHECK(read(header(7'999, TrackStatus::Decoding, 0)).has_value());
        CHECK(read(header(1'000'000, TrackStatus::Decoding, 0)).has_value());

        // Decoding, with frames ready.
        CHECK(read(header(44'100, TrackStatus::Decoding, 500)) ==
              TrackProgress{.sample_rate = 44'100,
                            .status = TrackStatus::Decoding,
                            .frames = 500,
                            .complete = false});

        // Each final status, complete.
        for (const TrackStatus status :
             {TrackStatus::Complete, TrackStatus::Truncated, TrackStatus::Failed}) {
            CAPTURE(static_cast<int>(status));
            CHECK(read(header(48'000, status, 700 | kTrackComplete)) ==
                  TrackProgress{
                      .sample_rate = 48'000, .status = status, .frames = 700, .complete = true});
            CHECK(read(header(48'000, status, kTrackComplete)) ==
                  TrackProgress{
                      .sample_rate = 48'000, .status = status, .frames = 0, .complete = true});
        }

        // The rate's limits.
        CHECK(read(header(kMinTrackRate, TrackStatus::Decoding, 1)).has_value());
        CHECK(read(header(kMaxTrackRate, TrackStatus::Decoding, 1)).has_value());
    }

    SECTION("frames are clamped to the capacity") {
        CHECK(read(header(44'100, TrackStatus::Decoding, 5'000)) ==
              TrackProgress{.sample_rate = 44'100,
                            .status = TrackStatus::Decoding,
                            .frames = kCapacity,
                            .complete = false});
        CHECK(read(header(44'100, TrackStatus::Complete, 5'000 | kTrackComplete)) ==
              TrackProgress{.sample_rate = 44'100,
                            .status = TrackStatus::Complete,
                            .frames = kCapacity,
                            .complete = true});
        // The most the low 63 bits can say.
        CHECK(read(header(44'100, TrackStatus::Decoding, kTrackComplete - 1)) ==
              TrackProgress{.sample_rate = 44'100,
                            .status = TrackStatus::Decoding,
                            .frames = kCapacity,
                            .complete = false});
        CHECK(read(header(44'100, TrackStatus::Decoding, 5'000), 0) ==
              TrackProgress{.sample_rate = 44'100,
                            .status = TrackStatus::Decoding,
                            .frames = 0,
                            .complete = false});
    }

    SECTION("bad rates") {
        for (const std::uint64_t rate : {std::uint64_t{0}, std::uint64_t{7'999},
                                         std::uint64_t{384'001}, std::uint64_t{1} << 32U}) {
            CAPTURE(rate);
            CHECK(refusal(header(rate, TrackStatus::Decoding, 1)) == TrackHeaderError::BadRate);
            CHECK(refusal(header(rate, TrackStatus::Complete, 1 | kTrackComplete)) ==
                  TrackHeaderError::BadRate);
            // Complete with no frames still needs a rate.
            CHECK(refusal(header(rate, TrackStatus::Failed, kTrackComplete)) ==
                  TrackHeaderError::BadRate);
        }
    }

    SECTION("bad statuses") {
        // Not a TrackStatus.
        for (const std::uint64_t status :
             {std::uint64_t{4}, std::uint64_t{255}, ~std::uint64_t{0}}) {
            CAPTURE(status);
            CHECK(refusal(TrackHeader{.sample_rate = 44'100, .status = status, .ready = 1}) ==
                  TrackHeaderError::BadStatus);
            CHECK(refusal(TrackHeader{.sample_rate = 44'100,
                                      .status = status,
                                      .ready = 1 | kTrackComplete}) == TrackHeaderError::BadStatus);
        }
        // Complete, but still Decoding.
        CHECK(refusal(header(44'100, TrackStatus::Decoding, 1 | kTrackComplete)) ==
              TrackHeaderError::BadStatus);
    }

    SECTION("an incomplete track reads as Decoding, whatever its status") {
        // The worker stores the final status just before it sets "complete".
        for (const TrackStatus status :
             {TrackStatus::Complete, TrackStatus::Truncated, TrackStatus::Failed}) {
            CAPTURE(static_cast<int>(status));
            CHECK(read(header(44'100, status, 1)) == TrackProgress{.sample_rate = 44'100,
                                                                   .status = TrackStatus::Decoding,
                                                                   .frames = 1,
                                                                   .complete = false});
            CHECK(read(header(44'100, status, 0)) == TrackProgress{.sample_rate = 44'100,
                                                                   .status = TrackStatus::Decoding,
                                                                   .frames = 0,
                                                                   .complete = false});
        }
    }

    SECTION("the fuzz target's check") {
        CHECK(check_track_input({}));
        for (const TrackHeader& checked :
             {TrackHeader{}, header(44'100, TrackStatus::Decoding, 500),
              header(44'100, TrackStatus::Complete, 5'000 | kTrackComplete),
              header(0, TrackStatus::Complete, kTrackComplete),
              header(44'100, TrackStatus::Decoding, 1 | kTrackComplete)}) {
            for (const std::uint64_t capacity :
                 {std::uint64_t{0}, kCapacity, kMaxTrackFrames, ~std::uint64_t{0}}) {
                const std::array<std::uint8_t, kTrackInputSize> input =
                    input_for(checked, capacity);
                CHECK(check_track_input(input));
            }
        }
    }
}

TEST_CASE("Track segment: writing", "[decode-worker]") {
    const std::unique_ptr<Segment> segment = make_track_segment();
    const std::span<std::byte> bytes = segment->region->bytes();
    REQUIRE(bytes.size() == kTrackSegmentSize);
    TrackWriter writer{bytes};
    CHECK(writer.capacity() == kMaxTrackFrames);
    REQUIRE(writer.samples().size() == 2 * kMaxTrackFrames);
    CHECK(static_cast<void*>(writer.samples().data()) ==
          static_cast<void*>(bytes.subspan(kTrackSamplesOffset).data()));

    // Nothing yet.
    CHECK(progress_of(*segment) ==
          TrackProgress{
              .sample_rate = 0, .status = TrackStatus::Decoding, .frames = 0, .complete = false});

    writer.start(44'100);
    CHECK(header_of(*segment).sample_rate == 44'100);
    CHECK(progress_of(*segment) == TrackProgress{.sample_rate = 44'100,
                                                 .status = TrackStatus::Decoding,
                                                 .frames = 0,
                                                 .complete = false});

    writer.samples()[0] = 0.25F;
    writer.samples()[1] = -0.5F;
    writer.publish(1);
    CHECK(header_of(*segment).ready == 1);
    CHECK(progress_of(*segment) == TrackProgress{.sample_rate = 44'100,
                                                 .status = TrackStatus::Decoding,
                                                 .frames = 1,
                                                 .complete = false});

    writer.publish(4'096);
    CHECK(progress_of(*segment) == TrackProgress{.sample_rate = 44'100,
                                                 .status = TrackStatus::Decoding,
                                                 .frames = 4'096,
                                                 .complete = false});

    writer.finish(TrackStatus::Truncated, 5'000);
    CHECK(header_of(*segment).status == static_cast<std::uint64_t>(TrackStatus::Truncated));
    CHECK(header_of(*segment).ready == (5'000 | kTrackComplete));
    CHECK(progress_of(*segment) == TrackProgress{.sample_rate = 44'100,
                                                 .status = TrackStatus::Truncated,
                                                 .frames = 5'000,
                                                 .complete = true});

    // The samples are where a reader looks for them.
    CHECK(ks::decode::test::samples_of(*segment)[0] == 0.25F);
    CHECK(ks::decode::test::samples_of(*segment)[1] == -0.5F);

    SECTION("a smaller segment's capacity comes from its size") {
        const std::unique_ptr<Segment> small =
            make_track_segment(kTrackSamplesOffset + (1'000 * ks::ipc::kTrackFrameSize));
        const TrackWriter small_writer{small->region->bytes()};
        CHECK(small_writer.capacity() == capacity_of(*small));
        CHECK(small_writer.capacity() >= 1'000);
        CHECK(small_writer.samples().size() == 2 * capacity_of(*small));
    }
}
