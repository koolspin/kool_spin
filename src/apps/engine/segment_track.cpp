// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine_app/segment_track.h>

#include <ks/engine/track.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/region.h>
#include <ks/ipc/track_segment.h>

#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace ks::engine_app {

namespace {

/// Why read_track() refused a header, for the log.
std::string reason(ipc::TrackHeaderError error) {
    switch (error) {
    case ipc::TrackHeaderError::BadRate:
        return "its rate is out of range";
    case ipc::TrackHeaderError::BadStatus:
        return "its status isn't a track status";
    }
    return "it can't be read";
}

/// The system's page size, or 4 KiB if it can't be read.
std::size_t system_page_size() noexcept {
    const long page = ::sysconf(_SC_PAGESIZE);
    return page > 0 ? static_cast<std::size_t>(page) : std::size_t{4'096};
}

} // namespace

std::expected<std::unique_ptr<SegmentTrack>, std::string>
SegmentTrack::map(ipc::Fd segment, std::uint64_t id, std::uint32_t sample_rate) {
    std::expected<ipc::Region, ipc::MapFailure> region = ipc::Region::map(
        segment.get(), ipc::SegmentKind::Track, ipc::kTrackLayoutVersion, ipc::Access::ReadOnly);
    if (!region) {
        return std::unexpected{std::string{"the segment isn't a track segment"}};
    }
    // The size of the engine's own mapping, never the header's.
    const std::span<std::byte> bytes = region->bytes();
    const auto* const header = region->at<ipc::TrackHeader>(ipc::kSegmentHeaderSize);
    const std::uint64_t capacity =
        bytes.size() < ipc::kTrackSamplesOffset
            ? 0
            : std::min<std::uint64_t>((bytes.size() - ipc::kTrackSamplesOffset) /
                                          ipc::kTrackFrameSize,
                                      ipc::kMaxTrackFrames);
    if (header == nullptr || capacity == 0) {
        return std::unexpected{std::string{"the segment holds no frames"}};
    }
    if (sample_rate == 0) {
        // One read; the header is another process's.
        const std::expected<ipc::TrackProgress, ipc::TrackHeaderError> progress =
            ipc::read_track(*header, capacity);
        if (!progress) {
            return std::unexpected{reason(progress.error())};
        }
        if (progress->sample_rate == 0) {
            return std::unexpected{std::string{"its rate isn't stored yet"}};
        }
        sample_rate = progress->sample_rate;
    }
    const std::span<const std::byte> sample_bytes = bytes.subspan(
        ipc::kTrackSamplesOffset, static_cast<std::size_t>(capacity) * ipc::kTrackFrameSize);
    const std::span<const float> samples{
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sample area holds floats
        reinterpret_cast<const float*>(sample_bytes.data()),
        static_cast<std::size_t>(capacity) * 2};
    return std::make_unique<SegmentTrack>(
        std::move(segment), std::move(*region), *header,
        std::make_unique<engine::Track>(samples, sample_rate, id));
}

SegmentTrack::SegmentTrack(ipc::Fd segment, ipc::Region region, const ipc::TrackHeader& header,
                           std::unique_ptr<engine::Track> track) noexcept
    : segment_{std::move(segment)}, region_{std::move(region)}, header_{&header},
      track_{std::move(track)}, page_size_{system_page_size()} {}

TrackPass SegmentTrack::pass(std::size_t budget) {
    if (finished_) {
        return {};
    }
    // Read once; everything below uses this copy (ADR 0003 §8).
    const std::expected<ipc::TrackProgress, ipc::TrackHeaderError> progress =
        ipc::read_track(*header_, track_->capacity());
    if (!progress) {
        return corrupt(reason(progress.error()));
    }
    if (progress->frames < passed_on_) {
        return corrupt(
            std::format("its frames went down from {} to {}", passed_on_, progress->frames));
    }
    if ((progress->frames > 0 || progress->complete) &&
        progress->sample_rate != track_->sample_rate()) {
        return corrupt(std::format("its rate is {} Hz, not the {} Hz it was loaded at",
                                   progress->sample_rate, track_->sample_rate()));
    }

    const std::uint64_t affordable = budget / ipc::kTrackFrameSize;
    const std::uint64_t target = passed_on_ + std::min(progress->frames - passed_on_, affordable);
    TrackPass result;
    if (target > passed_on_) {
        fault_in(passed_on_, target);
        result.faulted = static_cast<std::size_t>(target - passed_on_) * ipc::kTrackFrameSize;
    }
    const bool complete = progress->complete && target == progress->frames;
    if (target > passed_on_ || complete) {
        track_->publish(target, complete);
        passed_on_ = target;
    }
    if (complete) {
        finished_ = true;
        result.finished =
            TrackFinished{.frames = target, .status = progress->status, .corrupt = std::nullopt};
    }
    return result;
}

TrackPass SegmentTrack::corrupt(std::string why) {
    track_->publish(passed_on_, true);
    finished_ = true;
    return TrackPass{.faulted = 0,
                     .finished = TrackFinished{.frames = passed_on_,
                                               .status = ipc::TrackStatus::Failed,
                                               .corrupt = std::move(why)}};
}

void SegmentTrack::fault_in(std::uint64_t from, std::uint64_t to) const noexcept {
    const std::span<const std::byte> bytes = region_.bytes();
    const std::size_t begin =
        ipc::kTrackSamplesOffset + (static_cast<std::size_t>(from) * ipc::kTrackFrameSize);
    const std::size_t end =
        ipc::kTrackSamplesOffset + (static_cast<std::size_t>(to) * ipc::kTrackFrameSize);
    for (std::size_t page = begin / page_size_ * page_size_; page < end; page += page_size_) {
        // A volatile read, which the compiler must make: it maps the page in
        // this process, so the audio thread never takes the first fault on it.
        const volatile std::byte& first = bytes[page];
        static_cast<void>(first);
    }
}

} // namespace ks::engine_app
