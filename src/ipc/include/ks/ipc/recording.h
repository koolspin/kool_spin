// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The recording segment: what a test engine's paced backend played, in a
/// ring that outlives the engine, so chaos tests can check the output across
/// a `kill -9` (specs/engine-restart/spec.md requirements 14, 15, and 18).
///
/// The audio thread writes through a RecordingWriter. A test reads through a
/// RecordingReader, which copies, then checks that the writer hasn't
/// overwritten what it copied.

#include <ks/core/realtime.h>
#include <ks/ipc/layout.h>
#include <ks/ipc/region.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>
#include <vector>

namespace ks::ipc {

/// The layout version in a recording segment's header.
inline constexpr std::uint32_t kRecordingLayoutVersion = 1;

/// Frames the ring keeps: 2^22, about 95 s at 44.1 kHz.
inline constexpr std::size_t kRecordingFrames = std::size_t{1} << 22U;

/// Samples in a frame: the recording is always stereo.
inline constexpr std::size_t kRecordingChannels = 2;

/// Boundaries the segment keeps: one per engine start. Starts after the
/// 64th overwrite the last.
inline constexpr std::size_t kMaxBoundaries = 64;

/// Where an engine started recording.
struct RecordingBoundary {
    /// The segment's `frames_written` when the engine started.
    std::uint64_t frame;
    /// The engine's host time then: nanoseconds of std::chrono::steady_clock.
    std::uint64_t host_time;
};

/// What a recording segment holds after its header. Zero-filled memory is an
/// empty recording. Written only through RecordingWriter.
struct RecordingSegment {
    /// Total frames ever recorded, release-stored after each write's frames.
    alignas(kSharedAlignment) std::uint64_t frames_written;
    /// How many engines have started on the segment.
    alignas(kSharedAlignment) std::uint64_t starts;
    std::array<RecordingBoundary, kMaxBoundaries> boundaries;
    /// Frame n is at index n % kRecordingFrames, interleaved. Each sample is a
    /// float's bit pattern: shared layouts hold only integers.
    alignas(
        kSharedAlignment) std::array<std::uint32_t, kRecordingFrames * kRecordingChannels> samples;
};

static_assert(std::is_standard_layout_v<RecordingSegment>);
static_assert(std::is_trivially_copyable_v<RecordingSegment>);
static_assert(offsetof(RecordingSegment, frames_written) == 0);
static_assert(offsetof(RecordingSegment, starts) == kSharedAlignment);
static_assert(offsetof(RecordingSegment, boundaries) == kSharedAlignment + 8);
static_assert(offsetof(RecordingSegment, samples) % kSharedAlignment == 0);

/// A recording segment's size: the header, then a RecordingSegment at
/// kSegmentHeaderSize.
inline constexpr std::size_t kRecordingSegmentSize = kSegmentHeaderSize + sizeof(RecordingSegment);

static_assert(kSegmentHeaderSize % alignof(RecordingSegment) == 0);

/// The writing side (requirement 15).
class RecordingWriter {
public:
    /// Attaches to `segment`, which must outlive the writer, as an engine
    /// starting at `host_time`: records a boundary at the segment's
    /// `frames_written` (the last one's slot again after kMaxBoundaries),
    /// adds one to `starts`, and continues `frames_written` from there.
    RecordingWriter(RecordingSegment& segment, std::uint64_t host_time) noexcept;

    RecordingWriter(const RecordingWriter&) = delete;
    RecordingWriter& operator=(const RecordingWriter&) = delete;
    RecordingWriter(RecordingWriter&&) = delete;
    RecordingWriter& operator=(RecordingWriter&&) = delete;
    ~RecordingWriter() = default;

    /// Appends `interleaved`, which holds whole frames (a multiple of
    /// kRecordingChannels samples; a trailing part of a frame is ignored),
    /// then release-stores `frames_written`. Never waits, allocates, or
    /// reads the segment's counters back.
    void write(std::span<const float> interleaved) noexcept KS_NONBLOCKING;

    /// Total frames recorded, from the writer's own copy.
    [[nodiscard]] std::uint64_t frames_written() const noexcept KS_NONBLOCKING;

private:
    RecordingSegment* segment_;
    std::uint64_t frames_ = 0;
};

/// Why a read failed.
enum class RecordingError : std::uint8_t {
    /// The range isn't entirely written yet.
    NotYet,
    /// The writer has overwritten part of the range, or already had when the
    /// read began.
    Overwritten,
};

/// The reading side (requirement 15). Any thread, in any process, may read.
class RecordingReader {
public:
    /// Reads `segment`, which must outlive the reader.
    /// It's taken by non-const reference because std::atomic_ref needs one;
    /// the reader never writes.
    explicit RecordingReader(RecordingSegment& segment) noexcept;

    /// Frames recorded so far (acquire).
    [[nodiscard]] std::uint64_t frames_written() const noexcept;

    /// How many engines have started on the segment.
    [[nodiscard]] std::uint64_t starts() const noexcept;

    /// The boundaries so far, oldest first: at most kMaxBoundaries.
    [[nodiscard]] std::vector<RecordingBoundary> boundaries() const;

    /// Frames [first, first + count), interleaved (`count` × kRecordingChannels
    /// samples), copied, then checked: if the writer reached the range while
    /// it was copied, or before, the error is Overwritten; if the range
    /// extends past `frames_written`, NotYet.
    [[nodiscard]] std::expected<std::vector<float>, RecordingError> read(std::uint64_t first,
                                                                         std::uint64_t count) const;

private:
    RecordingSegment* segment_;
};

} // namespace ks::ipc
