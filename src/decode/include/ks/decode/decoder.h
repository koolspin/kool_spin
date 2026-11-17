// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Decoding audio files with FFmpeg (specs/decode-worker/spec.md requirements
/// 9 to 14): a file's best audio stream, as interleaved stereo 32-bit float
/// at the stream's own rate, and decoding it into a track segment. Only the
/// decode worker uses it (ADR 0002). Nothing here includes FFmpeg.

#include <ks/ipc/fd.h>
#include <ks/ipc/track_segment.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace ks::decode {

/// What a Decoder reads a file through (requirement 10).
class Source {
public:
    Source() = default;
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    Source(Source&&) = delete;
    Source& operator=(Source&&) = delete;
    virtual ~Source() = default;

    /// Reads up to buffer.size() bytes at `offset` into `buffer`. Returns the
    /// bytes read, fewer only at the end of the file and 0 past it, or an
    /// errno value.
    [[nodiscard]] virtual std::expected<std::size_t, int> read_at(std::uint64_t offset,
                                                                  std::span<std::byte> buffer) = 0;

    /// The file's size in bytes, or an errno value.
    [[nodiscard]] virtual std::expected<std::uint64_t, int> size() = 0;
};

/// Reads a descriptor with pread() and sizes it with fstat(), so the
/// descriptor's own offset doesn't matter. A descriptor that isn't a regular
/// file answers both with EINVAL.
class FdSource final : public Source {
public:
    explicit FdSource(ipc::Fd fd) noexcept;

    [[nodiscard]] std::expected<std::size_t, int> read_at(std::uint64_t offset,
                                                          std::span<std::byte> buffer) override;
    [[nodiscard]] std::expected<std::uint64_t, int> size() override;

private:
    ipc::Fd fd_;
};

/// Why Decoder::open() refused a file (requirement 11).
enum class RefusalReason : std::uint8_t {
    Unreadable,  ///< FFmpeg can't open it as a file it knows, or the source fails.
    NoAudio,     ///< It has no audio stream.
    Unsupported, ///< There's no decoder for its audio codec, or the decoder won't open.
    BadRate,     ///< Its rate is outside kMinTrackRate to kMaxTrackRate.
    BadChannels, ///< It has no channels, or more than 64.
    TooLong,     ///< Its estimated length is above kMaxTrackFrames.
};

/// A file Decoder::open() won't decode, and why, in one line.
struct Refusal {
    RefusalReason reason = RefusalReason::Unreadable;
    std::string text;
};

/// The most channels a file can have.
inline constexpr std::uint32_t kMaxChannels = 64;

/// What Decoder::open() found (requirement 12).
struct DecoderInfo {
    std::uint32_t sample_rate = 0;
    /// The stream's own channels, before mixing to stereo.
    std::uint32_t channels = 0;
    /// FFmpeg's duration for the stream, or else for the file, at the
    /// stream's rate, rounded to the nearest frame; 0 if it gives neither.
    std::uint64_t estimated_frames = 0;
    /// FFmpeg's name for the codec, such as "flac".
    std::string codec;
};

/// Why decoding stopped before the end of the file, in one line.
struct DecodeError {
    std::string text;

    friend bool operator==(const DecodeError&, const DecodeError&) = default;
};

/// A file's best audio stream, decoding (requirement 13). Movable, not
/// copyable.
class Decoder {
public:
    /// Reads enough of the file to find its best audio stream, and opens its
    /// decoder, or says why it won't.
    [[nodiscard]] static std::expected<Decoder, Refusal> open(std::unique_ptr<Source> source);

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&& other) noexcept;
    Decoder& operator=(Decoder&& other) noexcept;
    ~Decoder();

    [[nodiscard]] const DecoderInfo& info() const noexcept;

    /// Decodes into `out`, interleaved stereo, and returns the frames written:
    /// up to out.size() / 2, fewer only at the end of the file, and 0 at the
    /// end, every time after. Frames there's no room for are kept for the
    /// next call. After an error, every call returns the same error.
    [[nodiscard]] std::expected<std::size_t, DecodeError> read(std::span<float> out);

    /// The packets skipped because they didn't decode.
    [[nodiscard]] std::uint64_t skipped_packets() const noexcept;

private:
    struct State;
    explicit Decoder(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;
};

/// The most frames decode_track() asks read() for at a time.
inline constexpr std::size_t kDecodeChunkFrames = 4'096;

/// How decode_track() finished.
struct DecodedTrack {
    std::uint64_t frames = 0;
    ipc::TrackStatus status = ipc::TrackStatus::Failed;

    friend bool operator==(const DecodedTrack&, const DecodedTrack&) = default;
};

/// Decodes into a track segment (requirement 14): starts it with the
/// stream's rate, writes from frame 0 with at most kDecodeChunkFrames from
/// each read(), publishing after each read() that returns frames, and
/// finishes it. It stops when the file ends (Complete), at `max_frames` or
/// the writer's capacity, whichever is less (Truncated, unless the file ends
/// there too), when read() fails (Failed), or when `stopping()`, asked before
/// each read(), returns true (Failed). It writes nothing past the frames it
/// publishes.
[[nodiscard]] DecodedTrack decode_track(Decoder& decoder, ipc::TrackWriter& writer,
                                        std::uint64_t max_frames,
                                        const std::function<bool()>& stopping);

} // namespace ks::decode
