// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/ipc/recording.h>

#include <ks/ipc/layout.h>

#include <algorithm>
#include <atomic>
#include <bit>

namespace ks::ipc {

namespace {

/// The index in `samples` of channel `channel` of frame `frame`.
constexpr std::size_t index_of(std::uint64_t frame, std::size_t channel) noexcept {
    return ((static_cast<std::size_t>(frame % kRecordingFrames)) * kRecordingChannels) + channel;
}

} // namespace

RecordingWriter::RecordingWriter(RecordingSegment& segment, std::uint64_t host_time) noexcept
    : segment_{&segment}, frames_{load_shared(segment.frames_written, std::memory_order_relaxed)} {
    const std::uint64_t starts = load_shared(segment.starts, std::memory_order_relaxed);
    RecordingBoundary& boundary = std::span{segment.boundaries}[static_cast<std::size_t>(
        std::min<std::uint64_t>(starts, kMaxBoundaries - 1))];
    store_shared(boundary.frame, frames_, std::memory_order_relaxed);
    store_shared(boundary.host_time, host_time, std::memory_order_relaxed);
    store_shared(segment.starts, starts + 1, std::memory_order_release);
}

void RecordingWriter::write(std::span<const float> interleaved) noexcept KS_NONBLOCKING {
    const std::size_t frames = interleaved.size() / kRecordingChannels;
    const std::span<std::uint32_t> samples{segment_->samples};
    for (std::size_t i = 0; i < frames; ++i) {
        for (std::size_t channel = 0; channel < kRecordingChannels; ++channel) {
            std::atomic_ref{samples[index_of(frames_ + i, channel)]}.store(
                std::bit_cast<std::uint32_t>(interleaved[(i * kRecordingChannels) + channel]),
                std::memory_order_relaxed);
        }
    }
    frames_ += frames;
    store_shared(segment_->frames_written, frames_, std::memory_order_release);
}

std::uint64_t RecordingWriter::frames_written() const noexcept KS_NONBLOCKING {
    return frames_;
}

RecordingReader::RecordingReader(RecordingSegment& segment) noexcept : segment_{&segment} {}

std::uint64_t RecordingReader::frames_written() const noexcept {
    return load_shared(segment_->frames_written, std::memory_order_acquire);
}

std::uint64_t RecordingReader::starts() const noexcept {
    return load_shared(segment_->starts, std::memory_order_acquire);
}

std::vector<RecordingBoundary> RecordingReader::boundaries() const {
    const std::uint64_t starts_now = starts();
    const auto count =
        static_cast<std::size_t>(std::min<std::uint64_t>(starts_now, kMaxBoundaries));
    std::vector<RecordingBoundary> boundaries;
    boundaries.reserve(count);
    for (RecordingBoundary& boundary : std::span{segment_->boundaries}.first(count)) {
        boundaries.push_back(RecordingBoundary{
            .frame = load_shared(boundary.frame, std::memory_order_relaxed),
            .host_time = load_shared(boundary.host_time, std::memory_order_relaxed)});
    }
    return boundaries;
}

std::expected<std::vector<float>, RecordingError> RecordingReader::read(std::uint64_t first,
                                                                        std::uint64_t count) const {
    const std::uint64_t written = frames_written();
    if (count > written || first > written - count) {
        return std::unexpected{RecordingError::NotYet};
    }
    // The writer is at `written`, so only the newest kRecordingFrames are intact.
    const auto overtaken = [first](std::uint64_t at) {
        return at > kRecordingFrames && first < at - kRecordingFrames;
    };
    if (overtaken(written)) {
        return std::unexpected{RecordingError::Overwritten};
    }
    std::vector<float> samples;
    samples.reserve(static_cast<std::size_t>(count) * kRecordingChannels);
    const std::span<std::uint32_t> shared{segment_->samples};
    for (std::uint64_t frame = first; frame < first + count; ++frame) {
        for (std::size_t channel = 0; channel < kRecordingChannels; ++channel) {
            samples.push_back(std::bit_cast<float>(
                std::atomic_ref{shared[index_of(frame, channel)]}.load(std::memory_order_relaxed)));
        }
    }
    // Nothing the writer did while we copied may have reached the range.
    std::atomic_thread_fence(std::memory_order_acquire);
    if (overtaken(frames_written())) {
        return std::unexpected{RecordingError::Overwritten};
    }
    return samples;
}

} // namespace ks::ipc
