// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// A script's tracks: synthetic signals, decoded files, and the engine tracks
/// that hold them (specs/harness/spec.md requirements 20 to 23).

#include <ks/engine/track.h>
#include <ks/harness/script.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ks::harness {

/// Every signal's peak: −12 dBFS (requirement 23).
inline constexpr double kSignalPeakDbfs = -12.0;

/// Generates `signal` (requirement 23): interleaved stereo, the same on both
/// channels, at signal.sample_rate, llround(seconds × sample_rate) frames.
[[nodiscard]] std::vector<float> generate(const Signal& signal);

/// Audio decoded from a file.
struct DecodedAudio {
    /// The file's own sample rate.
    std::uint32_t sample_rate = 0;
    /// Interleaved stereo.
    std::vector<float> samples;
};

/// Decodes a WAV, FLAC, or MP3 file through the private codec library
/// (requirement 22). Fails, with a message that names the file, if it can't
/// be read or decoded, its sample rate is outside engine::kMinTrackRate to
/// engine::kMaxTrackRate, or it's longer than engine::kMaxTrackFrames.
[[nodiscard]] std::expected<DecodedAudio, std::string>
decode_file(const std::filesystem::path& path);

/// A script's tracks, ready to play (requirements 20 and 21). Tracks never
/// move once prepared, so the engine may hold references to them for as
/// long as the set lives.
class TrackSet {
public:
    /// Decodes or generates every track in `script`, in script order, and
    /// publishes each as complete, with ID index + 1. Fails, with a message
    /// that names the file, if a file track can't be decoded.
    [[nodiscard]] static std::expected<TrackSet, std::string> prepare(const Script& script);

    TrackSet(const TrackSet&) = delete;
    TrackSet& operator=(const TrackSet&) = delete;
    TrackSet(TrackSet&&) noexcept = default;
    TrackSet& operator=(TrackSet&&) noexcept = default;
    ~TrackSet() = default;

    [[nodiscard]] std::size_t size() const noexcept;

    /// Script track `index`, which has ID `index` + 1. `index` is below
    /// size().
    [[nodiscard]] const engine::Track& track(std::size_t index) const;

private:
    TrackSet() = default;

    /// Each track's samples. A vector's buffer stays put when the vector
    /// moves, so the tracks' spans stay valid.
    std::vector<std::vector<float>> samples_;
    std::vector<std::unique_ptr<engine::Track>> tracks_;
};

} // namespace ks::harness
