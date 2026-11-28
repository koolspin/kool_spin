// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// A track the engine plays from a track segment (specs/track-loading/spec.md
/// requirements 4 and 5; ADR 0003 §9): the segment, mapped read-only; the
/// engine::Track over its samples; and the control thread's work of passing
/// the worker's progress on to the audio thread.

#include <ks/engine/track.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/region.h>
#include <ks/ipc/track_segment.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace ks::engine_app {

/// How a track finished: what the control thread passes on no longer changes.
struct TrackFinished {
    /// The frames the track ends with.
    std::uint64_t frames = 0;
    /// How the worker ended it, if the header was sound.
    ipc::TrackStatus status = ipc::TrackStatus::Complete;
    /// Why the header is corrupt, if it is; then `status` means nothing.
    std::optional<std::string> corrupt;
};

/// What one pass over a track did.
struct TrackPass {
    /// Bytes of track memory faulted in.
    std::size_t faulted = 0;
    /// Set on the pass that finished the track.
    std::optional<TrackFinished> finished;
};

/// A track segment, mapped read-only, and the engine::Track built on it. The
/// control thread owns it; the audio thread reads only the Track.
class SegmentTrack {
public:
    /// Maps `segment` read-only as kind Track at kTrackLayoutVersion, and
    /// builds a Track over its sample area, with the capacity of the mapping
    /// (at most kMaxTrackFrames), `sample_rate`, and `id`. Nothing is ready.
    /// With a `sample_rate` of 0, the rate is the header's, which must be in
    /// range (a restarted engine's tracks, specs/engine-restart/spec.md
    /// requirement 5: the supervisor sent the rate with the LoadTrack, after
    /// the worker stored it). Says why if the segment is refused, or holds no
    /// frames.
    [[nodiscard]] static std::expected<std::unique_ptr<SegmentTrack>, std::string>
    map(ipc::Fd segment, std::uint64_t id, std::uint32_t sample_rate);

    /// Takes over a mapped segment, its header, and the Track on it. Use
    /// map().
    SegmentTrack(ipc::Fd segment, ipc::Region region, const ipc::TrackHeader& header,
                 std::unique_ptr<engine::Track> track) noexcept;

    SegmentTrack(const SegmentTrack&) = delete;
    SegmentTrack& operator=(const SegmentTrack&) = delete;
    SegmentTrack(SegmentTrack&&) = delete;
    SegmentTrack& operator=(SegmentTrack&&) = delete;
    /// Unmaps the segment and closes its descriptor.
    ~SegmentTrack() = default;

    [[nodiscard]] engine::Track& track() noexcept { return *track_; }
    [[nodiscard]] const engine::Track& track() const noexcept { return *track_; }
    [[nodiscard]] std::uint64_t id() const noexcept { return track_->id(); }

    /// True once the track is complete, or its header was found corrupt:
    /// pass() reads the header no more.
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    /// The frames passed on to the audio thread so far.
    [[nodiscard]] std::uint64_t passed_on() const noexcept { return passed_on_; }

    /// The bytes of frames pass() has locked in memory, and those it couldn't
    /// (specs/health-checks/spec.md requirement 2). Unmapping the track
    /// unlocks them.
    [[nodiscard]] std::uint64_t locked_bytes() const noexcept { return locked_bytes_; }
    [[nodiscard]] std::uint64_t unlocked_bytes() const noexcept { return unlocked_bytes_; }

    /// Reads the header once and passes its progress on (requirement 5):
    /// checks it; faults in the pages of newly published frames, at most
    /// `budget` bytes of them, and locks them in memory if it can
    /// (specs/health-checks/spec.md requirement 2, never retried); and
    /// publishes the frames faulted in, complete
    /// once the header is complete and every frame has been faulted in. A
    /// corrupt header finishes the track at the frames already passed on.
    /// Does nothing once the track is finished.
    TrackPass pass(std::size_t budget);

private:
    /// Finishes the track at passed_on_, as corrupt for `why`.
    TrackPass corrupt(std::string why);

    /// Reads a byte of every page that holds a byte of frames [from, to).
    void fault_in(std::uint64_t from, std::uint64_t to) const noexcept;

    /// Locks the pages that hold frames [from, to), and counts their bytes
    /// as locked or unlocked.
    void lock(std::uint64_t from, std::uint64_t to) noexcept;

    ipc::Fd segment_;
    ipc::Region region_;
    const ipc::TrackHeader* header_;
    std::unique_ptr<engine::Track> track_;
    std::size_t page_size_;
    std::uint64_t passed_on_ = 0;
    std::uint64_t locked_bytes_ = 0;
    std::uint64_t unlocked_bytes_ = 0;
    bool finished_ = false;
};

} // namespace ks::engine_app
