// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/engine-restart/spec.md: the recording segment's
// layout and its writer and reader (requirements 15 and 18).

#include <ks/ipc/recording.h>

#include <ks/ipc/region.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

using ks::ipc::kMaxBoundaries;
using ks::ipc::kRecordingChannels;
using ks::ipc::kRecordingFrames;
using ks::ipc::RecordingError;
using ks::ipc::RecordingReader;
using ks::ipc::RecordingSegment;
using ks::ipc::RecordingWriter;

/// Frame n of the test pattern: left n % 1000, right -(n % 1000).
float left(std::uint64_t n) {
    return static_cast<float>(n % 1'000);
}

/// Frames [first, first + count) of the pattern, interleaved.
std::vector<float> pattern(std::uint64_t first, std::uint64_t count) {
    std::vector<float> samples;
    for (std::uint64_t n = first; n < first + count; ++n) {
        samples.push_back(left(n));
        samples.push_back(-left(n));
    }
    return samples;
}

/// A zero-filled segment: valid, and empty.
std::unique_ptr<RecordingSegment> empty_segment() {
    return std::make_unique<RecordingSegment>();
}

} // namespace

TEST_CASE("Recording segment: layout, wrap, and slow readers", "[recording]") {
    SECTION("the layout") {
        CHECK(kRecordingFrames == (std::size_t{1} << 22U));
        CHECK(kRecordingChannels == 2);
        CHECK(kMaxBoundaries == 64);
        CHECK(offsetof(RecordingSegment, frames_written) == 0);
        CHECK(offsetof(RecordingSegment, starts) == ks::ipc::kSharedAlignment);
        CHECK(sizeof(ks::ipc::RecordingBoundary) == 16);
        CHECK(ks::ipc::kRecordingSegmentSize ==
              ks::ipc::kSegmentHeaderSize + sizeof(RecordingSegment));
        CHECK(static_cast<int>(ks::ipc::SegmentKind::Recording) == 5);
        CHECK(ks::ipc::kRecordingLayoutVersion == 1);
    }

    SECTION("zero-filled memory is an empty recording") {
        const auto segment = empty_segment();
        const RecordingReader reader{*segment};
        CHECK(reader.frames_written() == 0);
        CHECK(reader.starts() == 0);
        CHECK(reader.boundaries().empty());
        CHECK(reader.read(0, 1) == std::unexpected{RecordingError::NotYet});
        // An empty range is always there.
        const auto nothing = reader.read(0, 0);
        REQUIRE(nothing.has_value());
        CHECK(nothing->empty());
    }

    SECTION("a writer's frames appear, and an engine start is a boundary") {
        const auto segment = empty_segment();
        RecordingWriter writer{*segment, 1'000};
        const RecordingReader reader{*segment};
        CHECK(reader.starts() == 1);
        REQUIRE(reader.boundaries().size() == 1);
        CHECK(reader.boundaries().front().frame == 0);
        CHECK(reader.boundaries().front().host_time == 1'000);

        writer.write(pattern(0, 3));
        CHECK(writer.frames_written() == 3);
        CHECK(reader.frames_written() == 3);
        CHECK(reader.read(0, 3) == pattern(0, 3));
        CHECK(reader.read(1, 2) == pattern(1, 2));
        CHECK(reader.read(1, 3) == std::unexpected{RecordingError::NotYet});
        CHECK(reader.read(4, 1) == std::unexpected{RecordingError::NotYet});

        // A part of a frame at the end is ignored.
        std::vector<float> odd = pattern(3, 2);
        odd.push_back(99.0F);
        writer.write(odd);
        CHECK(reader.frames_written() == 5);
        CHECK(reader.read(0, 5) == pattern(0, 5));

        // An empty write changes nothing.
        writer.write({});
        CHECK(reader.frames_written() == 5);
    }

    SECTION("a second engine carries on, and the boundaries show where") {
        const auto segment = empty_segment();
        {
            RecordingWriter first{*segment, 1'000};
            first.write(pattern(0, 7));
        }
        RecordingWriter second{*segment, 5'000};
        const RecordingReader reader{*segment};
        CHECK(second.frames_written() == 7);
        CHECK(reader.starts() == 2);
        const auto boundaries = reader.boundaries();
        REQUIRE(boundaries.size() == 2);
        CHECK(boundaries[0].frame == 0);
        CHECK(boundaries[0].host_time == 1'000);
        CHECK(boundaries[1].frame == 7);
        CHECK(boundaries[1].host_time == 5'000);
        second.write(pattern(7, 3));
        CHECK(reader.read(0, 10) == pattern(0, 10));
    }

    SECTION("starts after the 64th overwrite the last boundary") {
        const auto segment = empty_segment();
        for (std::uint64_t i = 1; i <= kMaxBoundaries + 6; ++i) {
            const RecordingWriter writer{*segment, i * 10};
        }
        const RecordingReader reader{*segment};
        CHECK(reader.starts() == kMaxBoundaries + 6);
        const auto boundaries = reader.boundaries();
        REQUIRE(boundaries.size() == kMaxBoundaries);
        CHECK(boundaries.front().host_time == 10);
        CHECK(boundaries.back().host_time == (kMaxBoundaries + 6) * 10);
    }

    SECTION("the ring keeps the newest kRecordingFrames") {
        const auto segment = empty_segment();
        RecordingWriter writer{*segment, 1};
        const RecordingReader reader{*segment};
        constexpr std::uint64_t kChunk = 4'096;
        constexpr std::uint64_t kTotal = kRecordingFrames + 10;
        for (std::uint64_t n = 0; n < kTotal; n += kChunk) {
            writer.write(pattern(n, std::min(kChunk, kTotal - n)));
        }
        CHECK(reader.frames_written() == kTotal);
        CHECK(reader.read(0, 1) == std::unexpected{RecordingError::Overwritten});
        CHECK(reader.read(9, 1) == std::unexpected{RecordingError::Overwritten});
        CHECK(reader.read(0, kTotal) == std::unexpected{RecordingError::Overwritten});
        // The oldest frame still there is 10.
        CHECK(reader.read(10, 1) == pattern(10, 1));
        const auto all = reader.read(10, kRecordingFrames);
        REQUIRE(all.has_value());
        CHECK(*all == pattern(10, kRecordingFrames));
    }

    SECTION("a reader that the writer overtakes is told, never given wrong frames") {
        const auto segment = empty_segment();
        RecordingWriter writer{*segment, 1};
        const RecordingReader reader{*segment};
        std::atomic<bool> stop{false};
        std::thread thread{[&] {
            std::uint64_t n = 0;
            constexpr std::uint64_t kChunk = 512;
            while (!stop.load(std::memory_order_relaxed)) {
                writer.write(pattern(n, kChunk));
                n += kChunk;
            }
        }};
        std::uint64_t good = 0;
        std::uint64_t told = 0;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < end) {
            const std::uint64_t written = reader.frames_written();
            // Mostly near the writer, where it can overtake the read; and
            // sometimes far behind, where it already has.
            std::uint64_t first = written > 2'048 ? written - 2'048 : 0;
            if (good % 4 == 0 && written > kRecordingFrames / 2) {
                first = written - kRecordingFrames + 8'192;
            }
            const auto read = reader.read(first, 1'024);
            if (read) {
                ++good;
                REQUIRE(*read == pattern(first, 1'024));
            } else {
                ++told;
            }
        }
        stop = true;
        thread.join();
        CHECK(good > 0);
        static_cast<void>(told);
    }
}
