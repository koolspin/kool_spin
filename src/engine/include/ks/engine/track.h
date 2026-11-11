// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Track memory (ADR 0003 §9): a track's decoded samples, and how far
/// decoding has got.

#include <ks/core/realtime.h>

#include <atomic>
#include <cstdint>
#include <span>

namespace ks::engine {

/// The lowest track sample rate a deck accepts.
inline constexpr std::uint32_t kMinTrackRate = 8'000;
/// The highest track sample rate a deck accepts.
inline constexpr std::uint32_t kMaxTrackRate = 384'000;
/// The largest capacity a deck accepts, in frames: 2 GiB of samples.
inline constexpr std::uint64_t kMaxTrackFrames = std::uint64_t{1} << 28U;

/// How much of a track is decoded.
struct TrackReady {
    /// Frames [0, frames) are decoded and can be read.
    std::uint64_t frames = 0;
    /// No more frames will come.
    bool complete = false;
};

/// A view of a track's memory: interleaved stereo 32-bit float frames at the
/// track's own sample rate. Whatever decodes the track publishes its progress;
/// decks read only the frames published as ready.
class Track {
public:
    /// `samples` is interleaved stereo, so the capacity is samples.size() / 2
    /// frames. The memory must outlive the track. Nothing is ready yet.
    Track(std::span<const float> samples, std::uint32_t sample_rate) noexcept;

    Track(const Track&) = delete;
    Track& operator=(const Track&) = delete;
    Track(Track&&) = delete;
    Track& operator=(Track&&) = delete;
    ~Track() = default;

    /// Publishes that frames [0, frames) are decoded, and whether that's all
    /// of them, in one store with release ordering. Called by whatever decodes
    /// the track.
    void publish(std::uint64_t frames, bool complete) noexcept KS_NONBLOCKING;

    /// Decoding progress, from one load with acquire ordering. The frame count
    /// is clamped to the capacity.
    [[nodiscard]] TrackReady ready() const noexcept KS_NONBLOCKING;

    /// All of the track's memory, ready or not.
    [[nodiscard]] std::span<const float> samples() const noexcept KS_NONBLOCKING;

    /// The frames the memory holds: samples().size() / 2.
    [[nodiscard]] std::uint64_t capacity() const noexcept KS_NONBLOCKING;

    [[nodiscard]] std::uint32_t sample_rate() const noexcept KS_NONBLOCKING;

private:
    std::span<const float> samples_;
    std::uint32_t sample_rate_;
    /// The frame count in the low 63 bits, and the complete flag in the top bit.
    std::atomic<std::uint64_t> ready_{0};
};

} // namespace ks::engine
