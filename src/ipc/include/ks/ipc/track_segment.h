// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The track segment's layout (ADR 0003 §9; specs/decode-worker/spec.md
/// requirements 3 to 6): a header that the decode worker writes, then a
/// track's decoded samples, interleaved stereo 32-bit float at the track's own
/// rate. The worker is its only writer.

#include <ks/core/realtime.h>
#include <ks/ipc/region.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>

namespace ks::ipc {

/// The layout version in a track segment's header.
inline constexpr std::uint32_t kTrackLayoutVersion = 1;

/// The lowest sample rate a track can have, in Hz.
inline constexpr std::uint32_t kMinTrackRate = 8'000;
/// The highest sample rate a track can have, in Hz.
inline constexpr std::uint32_t kMaxTrackRate = 384'000;
/// The most frames a track can hold: 2 GiB of interleaved stereo floats.
inline constexpr std::uint64_t kMaxTrackFrames = std::uint64_t{1} << 28U;

/// Bytes in one frame: two 32-bit floats.
inline constexpr std::size_t kTrackFrameSize = 2 * sizeof(float);
/// Where the samples start: the largest page size on Linux and macOS, so they
/// start on a page.
inline constexpr std::size_t kTrackSamplesOffset = 16'384;
/// Every track segment's size, in bytes.
inline constexpr std::size_t kTrackSegmentSize =
    kTrackSamplesOffset + (static_cast<std::size_t>(kMaxTrackFrames) * kTrackFrameSize);

/// The "complete" bit in TrackHeader::ready: no more frames will come.
inline constexpr std::uint64_t kTrackComplete = std::uint64_t{1} << 63U;

/// How decoding a track ended, or that it hasn't.
enum class TrackStatus : std::uint8_t {
    Decoding = 0,  ///< Not finished.
    Complete = 1,  ///< The file ended.
    Truncated = 2, ///< Decoding stopped at its limit.
    Failed = 3,    ///< An error or a stop ended decoding; the frames before it are good.
};

/// The header at kSegmentHeaderSize. A zero-filled header is a track not
/// started yet. Each field is written through store_shared() and read through
/// load_shared(): another process may be writing it.
struct TrackHeader {
    /// The track's rate in Hz. Written before the first store to `ready`, and
    /// not changed after.
    std::uint64_t sample_rate;
    /// A TrackStatus, kept as an integer: another process wrote it. Written
    /// before `ready` gets kTrackComplete.
    std::uint64_t status;
    /// Frames decoded, in the low 63 bits, and kTrackComplete once no more
    /// will come. Stored with release ordering, only ever growing.
    std::uint64_t ready;
};

static_assert(std::is_standard_layout_v<TrackHeader> && std::is_trivially_copyable_v<TrackHeader>);
static_assert(kSegmentHeaderSize + sizeof(TrackHeader) <= kTrackSamplesOffset);

/// A track's header, read once and checked.
struct TrackProgress {
    std::uint32_t sample_rate = 0;
    TrackStatus status = TrackStatus::Decoding;
    /// Frames decoded, clamped to the reader's capacity.
    std::uint64_t frames = 0;
    /// No more frames will come.
    bool complete = false;

    friend bool operator==(const TrackProgress&, const TrackProgress&) = default;
};

/// Why read_track() refused a header.
enum class TrackHeaderError : std::uint8_t {
    /// Frames are ready, or the track is complete, and its rate is outside
    /// kMinTrackRate to kMaxTrackRate.
    BadRate,
    /// The status isn't a TrackStatus, or doesn't agree with "complete".
    BadStatus,
};

/// Reads `header` once, as another process may be writing it: `ready` with
/// acquire ordering first, then the rest, relaxed. `capacity` is the frames
/// the caller's own mapping holds; the frame count is clamped to it. A header
/// with no frames, not complete, and status Decoding is valid whatever its
/// rate: the worker hasn't started.
[[nodiscard]] std::expected<TrackProgress, TrackHeaderError> read_track(const TrackHeader& header,
                                                                        std::uint64_t capacity);

/// Writes a track segment that the caller mapped read-write. Not copyable: it
/// keeps the frames it has published, which only grow.
class TrackWriter {
public:
    /// `segment` is the whole mapping, at least kTrackSamplesOffset bytes. The
    /// capacity is (segment.size() - kTrackSamplesOffset) / kTrackFrameSize
    /// frames, at most kMaxTrackFrames.
    explicit TrackWriter(std::span<std::byte> segment) noexcept;

    TrackWriter(const TrackWriter&) = delete;
    TrackWriter& operator=(const TrackWriter&) = delete;
    TrackWriter(TrackWriter&&) = delete;
    TrackWriter& operator=(TrackWriter&&) = delete;
    ~TrackWriter() = default;

    /// The frames the segment holds.
    [[nodiscard]] std::uint64_t capacity() const noexcept;

    /// Stores the track's rate. Call it before the first publish().
    void start(std::uint32_t sample_rate) noexcept;

    /// The sample area: two floats per frame of capacity.
    [[nodiscard]] std::span<float> samples() const noexcept;

    /// Stores `frames` in `ready`, with release ordering.
    /// @pre frames is at least the last count published, and at most the
    ///      capacity.
    void publish(std::uint64_t frames) noexcept;

    /// Stores `status`, then `frames | kTrackComplete` in `ready`, with
    /// release ordering.
    /// @pre status isn't Decoding; frames is as for publish().
    void finish(TrackStatus status, std::uint64_t frames) noexcept;

private:
    TrackHeader* header_ = nullptr;
    std::span<float> samples_;
};

} // namespace ks::ipc
