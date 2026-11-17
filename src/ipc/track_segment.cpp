// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/track_segment.h>

#include <ks/ipc/layout.h>
#include <ks/ipc/region.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace ks::ipc {

namespace {

/// True if `rate` is a rate a track can have.
bool valid_rate(std::uint64_t rate) noexcept {
    return rate >= kMinTrackRate && rate <= kMaxTrackRate;
}

} // namespace

std::expected<TrackProgress, TrackHeaderError> read_track(const TrackHeader& header,
                                                          std::uint64_t capacity) {
    // load_shared() takes a reference it could write through, but only loads:
    // the header may be mapped read-only.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): only loaded
    auto& words = const_cast<TrackHeader&>(header);
    // `ready` first, with acquire ordering: the worker stores the rate and the
    // status before the `ready` that publishes them.
    const std::uint64_t ready = load_shared(words.ready, std::memory_order_acquire);
    const std::uint64_t status = load_shared(words.status, std::memory_order_relaxed);
    const std::uint64_t rate = load_shared(words.sample_rate, std::memory_order_relaxed);

    const bool complete = (ready & kTrackComplete) != 0;
    const std::uint64_t frames = ready & ~kTrackComplete;
    const bool finished = status == static_cast<std::uint64_t>(TrackStatus::Complete) ||
                          status == static_cast<std::uint64_t>(TrackStatus::Truncated) ||
                          status == static_cast<std::uint64_t>(TrackStatus::Failed);
    const bool decoding = status == static_cast<std::uint64_t>(TrackStatus::Decoding);
    if (complete ? !finished : !decoding) {
        return std::unexpected{TrackHeaderError::BadStatus};
    }
    // Any frame published, or the end, needs a rate to play at.
    if ((frames > 0 || complete) && !valid_rate(rate)) {
        return std::unexpected{TrackHeaderError::BadRate};
    }
    return TrackProgress{
        // A worker that hasn't started may have no rate yet.
        .sample_rate = valid_rate(rate) ? static_cast<std::uint32_t>(rate) : 0,
        .status = static_cast<TrackStatus>(status),
        .frames = std::min(frames, capacity),
        .complete = complete,
    };
}

TrackWriter::TrackWriter(std::span<std::byte> segment) noexcept {
    if (segment.size() < kTrackSamplesOffset) {
        return;
    }
    // The segment is page-aligned, so the header and the samples are aligned
    // for their types.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a shared layout
    header_ = reinterpret_cast<TrackHeader*>(segment.subspan(kSegmentHeaderSize).data());
    const std::span<std::byte> samples = segment.subspan(kTrackSamplesOffset);
    const std::uint64_t frames =
        std::min<std::uint64_t>(samples.size() / kTrackFrameSize, kMaxTrackFrames);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sample area holds floats
    samples_ = {reinterpret_cast<float*>(samples.data()), static_cast<std::size_t>(frames) * 2};
}

std::uint64_t TrackWriter::capacity() const noexcept {
    return samples_.size() / 2;
}

void TrackWriter::start(std::uint32_t sample_rate) noexcept {
    if (header_ != nullptr) {
        store_shared(header_->sample_rate, sample_rate, std::memory_order_relaxed);
    }
}

std::span<float> TrackWriter::samples() const noexcept {
    return samples_;
}

void TrackWriter::publish(std::uint64_t frames) noexcept {
    if (header_ != nullptr) {
        // Release: the frames, and the rate before them, are written first.
        store_shared(header_->ready, frames, std::memory_order_release);
    }
}

void TrackWriter::finish(TrackStatus status, std::uint64_t frames) noexcept {
    if (header_ != nullptr) {
        store_shared(header_->status, static_cast<std::uint64_t>(status),
                     std::memory_order_relaxed);
        store_shared(header_->ready, frames | kTrackComplete, std::memory_order_release);
    }
}

} // namespace ks::ipc
