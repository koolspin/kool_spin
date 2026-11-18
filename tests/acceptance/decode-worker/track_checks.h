// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The track segment fuzz target's check (specs/decode-worker/spec.md
// requirement 7). It lives in a header so that "Track segment: reading the
// header" compiles, lints, and runs it in every preset; the fuzz target runs
// it on arbitrary input.

#include <ks/ipc/track_segment.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>

namespace ks::decode::test {

/// The bytes check_track_input() reads: a TrackHeader, then a capacity.
inline constexpr std::size_t kTrackInputSize = sizeof(ipc::TrackHeader) + sizeof(std::uint64_t);

/// Builds a TrackHeader and a capacity (up to kMaxTrackFrames) from `input`,
/// zero-padded, and reads the header. True if read_track() refused it, or
/// what it returned satisfies requirement 5: frames at most the capacity, a
/// rate in range when frames are ready or the track is complete, and a status
/// that agrees with "complete".
inline bool check_track_input(std::span<const std::uint8_t> input) {
    std::array<std::uint8_t, kTrackInputSize> bytes{};
    std::copy_n(input.begin(), std::min(input.size(), kTrackInputSize), bytes.begin());
    ipc::TrackHeader header{};
    std::memcpy(&header, bytes.data(), sizeof header);
    std::uint64_t capacity = 0;
    std::memcpy(&capacity, std::span{bytes}.subspan(sizeof header).data(), sizeof capacity);
    capacity %= ipc::kMaxTrackFrames + 1;

    const std::expected<ipc::TrackProgress, ipc::TrackHeaderError> progress =
        ipc::read_track(header, capacity);
    if (!progress) {
        return true;
    }
    if (progress->frames > capacity) {
        return false;
    }
    if ((progress->frames > 0 || progress->complete) &&
        (progress->sample_rate < ipc::kMinTrackRate ||
         progress->sample_rate > ipc::kMaxTrackRate)) {
        return false;
    }
    switch (progress->status) {
    case ipc::TrackStatus::Decoding:
        return !progress->complete;
    case ipc::TrackStatus::Complete:
    case ipc::TrackStatus::Truncated:
    case ipc::TrackStatus::Failed:
        return progress->complete;
    }
    return false;
}

} // namespace ks::decode::test
