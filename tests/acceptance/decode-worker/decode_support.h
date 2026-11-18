// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the decode-worker acceptance tests: the fixtures, WAV files the
// tests write, opening decoders, measuring tones, and track segments.

#include <ks/decode/decoder.h>
#include <ks/dsp/frequency_meter.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/region.h>
#include <ks/ipc/segment.h>
#include <ks/ipc/track_segment.h>

#include "segments_support.h"
#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ks::decode::test {

/// The fixtures made by data/make.sh.
inline std::filesystem::path fixture(std::string_view name) {
    return std::filesystem::path{KS_DECODE_TEST_DATA} / name;
}

/// The frames in each 1-second fixture at 44.1 kHz.
inline constexpr std::size_t kSecond = 44'100;

/// Opens `path` for reading, close-on-exec. The Fd holds -1 if it can't.
inline ipc::Fd open_file(const std::filesystem::path& path) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open() is variadic
    return ipc::Fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
}

/// A source reading `path`.
inline std::unique_ptr<Source> source_for(const std::filesystem::path& path) {
    return std::make_unique<FdSource>(open_file(path));
}

/// The refusal's text, or empty if it opened.
inline std::string refusal_text(const std::expected<Decoder, Refusal>& decoder) {
    return decoder ? std::string{} : decoder.error().text;
}

/// A decoder on `source`, which must open.
inline Decoder open_decoder(std::unique_ptr<Source> source) {
    std::expected<Decoder, Refusal> decoder = Decoder::open(std::move(source));
    CAPTURE(refusal_text(decoder));
    REQUIRE(decoder.has_value());
    return std::move(*decoder);
}

/// A decoder on the file at `path`, which must open.
inline Decoder open_decoder(const std::filesystem::path& path) {
    return open_decoder(source_for(path));
}

/// Why a decoder on `path` was refused, or nothing if it opened.
inline std::optional<RefusalReason> refusal_of(const std::filesystem::path& path) {
    const std::expected<Decoder, Refusal> decoder = Decoder::open(source_for(path));
    if (decoder) {
        return std::nullopt;
    }
    return decoder.error().reason;
}

/// Everything `decoder` decodes, `chunk` frames at a time, until read()
/// returns 0. Requires no error, and every read but the last to fill `out`.
inline std::vector<float> decode_all(Decoder& decoder, std::size_t chunk = kDecodeChunkFrames) {
    std::vector<float> samples;
    std::vector<float> out(chunk * 2);
    for (;;) {
        const std::expected<std::size_t, DecodeError> frames = decoder.read(out);
        CAPTURE(frames ? std::string{} : frames.error().text);
        REQUIRE(frames.has_value());
        if (*frames == 0) {
            return samples;
        }
        REQUIRE(*frames <= chunk);
        samples.insert(samples.end(), out.begin(),
                       out.begin() + static_cast<std::ptrdiff_t>(*frames * 2));
        if (*frames < chunk) {
            // Fewer only at the end: the next read returns 0.
            const std::expected<std::size_t, DecodeError> after = decoder.read(out);
            REQUIRE(after == std::size_t{0});
            return samples;
        }
    }
}

/// The frames in interleaved stereo `samples`.
inline std::size_t frames_in(std::span<const float> samples) {
    return samples.size() / 2;
}

/// One channel (0 left, 1 right) of interleaved stereo `samples`, frames
/// [from, to).
inline std::vector<float> channel_of(std::span<const float> samples, std::size_t channel,
                                     std::size_t from, std::size_t to) {
    std::vector<float> out;
    to = std::min(to, frames_in(samples));
    for (std::size_t frame = from; frame < to; ++frame) {
        out.push_back(samples[(frame * 2) + channel]);
    }
    return out;
}

/// The frequency of `signal`, measured from its zero crossings.
inline double frequency_of(std::span<const float> signal, double sample_rate) {
    dsp::FrequencyMeter meter{sample_rate};
    for (const float sample : signal) {
        meter.push(sample);
    }
    return meter.frequency_hz();
}

/// The RMS level of `signal` in dBFS, or -1000 for silence.
inline double rms_db(std::span<const float> signal) {
    double sum = 0.0;
    for (const float sample : signal) {
        sum += static_cast<double>(sample) * static_cast<double>(sample);
    }
    if (signal.empty() || sum == 0.0) {
        return -1000.0;
    }
    return 10.0 * std::log10(sum / static_cast<double>(signal.size()));
}

/// The RMS level of a 1 kHz sine at -6 dBFS, as data/make.sh makes it.
inline const double kToneRmsDb = 20.0 * std::log10(0.5 / std::numbers::sqrt2);

/// Appends `value` to `bytes`, little-endian, in `size` bytes.
inline void append_le(std::vector<char>& bytes, std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        bytes.push_back(static_cast<char>(value & 0xFFU));
        value >>= 8U;
    }
}

/// A WAV header for `frames` frames: PCM (format 1) or IEEE float (format 3).
inline std::vector<char> wav_header(std::uint32_t sample_rate, std::uint16_t channels,
                                    std::uint16_t format, std::uint16_t bits,
                                    std::uint64_t frames) {
    const std::uint64_t block = std::uint64_t{channels} * (bits / 8U);
    const std::uint64_t data_size = frames * block;
    std::vector<char> header;
    header.insert(header.end(), {'R', 'I', 'F', 'F'});
    append_le(header, 36 + data_size, 4);
    header.insert(header.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    append_le(header, 16, 4);
    append_le(header, format, 2);
    append_le(header, channels, 2);
    append_le(header, sample_rate, 4);
    append_le(header, sample_rate * block, 4);
    append_le(header, block, 2);
    append_le(header, bits, 2);
    header.insert(header.end(), {'d', 'a', 't', 'a'});
    append_le(header, data_size, 4);
    return header;
}

/// Writes a 16-bit PCM WAV file of interleaved `samples`.
inline void write_wav_s16(const std::filesystem::path& path, std::uint32_t sample_rate,
                          std::uint16_t channels, std::span<const std::int16_t> samples) {
    std::vector<char> bytes = wav_header(sample_rate, channels, 1, 16, samples.size() / channels);
    for (const std::int16_t sample : samples) {
        append_le(bytes, static_cast<std::uint16_t>(sample), 2);
    }
    std::ofstream file{path, std::ios::binary};
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
}

/// Writes a 32-bit float WAV file of interleaved `samples`.
inline void write_wav_f32(const std::filesystem::path& path, std::uint32_t sample_rate,
                          std::uint16_t channels, std::span<const float> samples) {
    std::vector<char> bytes = wav_header(sample_rate, channels, 3, 32, samples.size() / channels);
    for (const float sample : samples) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &sample, sizeof bits);
        append_le(bytes, bits, 4);
    }
    std::ofstream file{path, std::ios::binary};
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
}

/// A 1 kHz sine at -6 dBFS, `frames` long, the same in every channel.
inline std::vector<float> tone(std::uint32_t sample_rate, std::uint16_t channels,
                               std::size_t frames) {
    std::vector<float> samples;
    samples.reserve(frames * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double phase = 2.0 * std::numbers::pi * 1'000.0 * static_cast<double>(frame) /
                             static_cast<double>(sample_rate);
        const auto value = static_cast<float>(0.5 * std::sin(phase));
        for (std::uint16_t channel = 0; channel < channels; ++channel) {
            samples.push_back(value);
        }
    }
    return samples;
}

/// The 16-bit tone, rounded.
inline std::vector<std::int16_t> to_s16(std::span<const float> samples) {
    std::vector<std::int16_t> out;
    out.reserve(samples.size());
    for (const float sample : samples) {
        out.push_back(static_cast<std::int16_t>(std::lround(sample * 32'767.0F)));
    }
    return out;
}

/// The long WAV's sample for `frame` and `channel` (0 or 1): every frame
/// differs from its neighbours, and the pattern doesn't repeat for 65,536
/// frames.
inline std::int16_t long_sample(std::uint64_t frame, std::size_t channel) {
    const std::uint64_t value = channel == 0 ? (frame * 7U) + 3U : (frame * 13U) + 5U;
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(value & 0xFFFFU));
}

/// The frames in the long WAV: 10 minutes at 44.1 kHz.
inline constexpr std::uint64_t kLongFrames = std::uint64_t{600} * kSecond;

/// Writes the long WAV: stereo, 16-bit, 44.1 kHz, kLongFrames frames of
/// long_sample().
inline void write_long_wav(const std::filesystem::path& path) {
    std::ofstream file{path, std::ios::binary};
    const std::vector<char> header = wav_header(44'100, 2, 1, 16, kLongFrames);
    file.write(header.data(), static_cast<std::streamsize>(header.size()));
    constexpr std::uint64_t kChunk = 65'536;
    std::vector<std::uint16_t> chunk;
    chunk.reserve(kChunk * 2);
    for (std::uint64_t start = 0; start < kLongFrames; start += kChunk) {
        chunk.clear();
        const std::uint64_t end = std::min(start + kChunk, kLongFrames);
        for (std::uint64_t frame = start; frame < end; ++frame) {
            chunk.push_back(static_cast<std::uint16_t>(long_sample(frame, 0)));
            chunk.push_back(static_cast<std::uint16_t>(long_sample(frame, 1)));
        }
        // WAV is little-endian, as Linux and macOS are on x86-64 and arm64.
        const std::span<const std::byte> bytes = std::as_bytes(std::span{chunk});
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): ostream writes chars
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    static_assert(std::endian::native == std::endian::little);
    REQUIRE(file.good());
}

/// True if frame `frame` of `samples` holds the long WAV's values.
inline bool long_frame_right(std::span<const float> samples, std::uint64_t frame) {
    const std::size_t index = static_cast<std::size_t>(frame) * 2;
    return samples[index] == static_cast<float>(long_sample(frame, 0)) / 32'768.0F &&
           samples[index + 1] == static_cast<float>(long_sample(frame, 1)) / 32'768.0F;
}

/// A copy of the fixture `name` at `path`, with `change` applied to its
/// bytes.
inline void write_changed_copy(std::string_view name, const std::filesystem::path& path,
                               const std::function<void(std::vector<char>&)>& change) {
    std::ifstream in{fixture(name), std::ios::binary};
    std::vector<char> bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    REQUIRE(!bytes.empty());
    change(bytes);
    std::ofstream out{path, std::ios::binary};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

/// Writes tone.flac with its STREAMINFO claiming `frames` frames.
inline void write_long_claiming_flac(const std::filesystem::path& path, std::uint64_t frames) {
    write_changed_copy("tone.flac", path, [frames](std::vector<char>& bytes) {
        REQUIRE(std::string_view{bytes.data(), 4} == "fLaC");
        // STREAMINFO's body starts at byte 8. Its bytes 10 to 17 hold the
        // rate (20 bits), channels (3), bits per sample (5), and total
        // samples (36), big-endian.
        constexpr std::size_t kAt = 8 + 10;
        std::uint64_t packed = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            packed = (packed << 8U) | static_cast<std::uint8_t>(bytes[kAt + i]);
        }
        constexpr std::uint64_t kTotalMask = (std::uint64_t{1} << 36U) - 1;
        packed = (packed & ~kTotalMask) | (frames & kTotalMask);
        for (std::size_t i = 0; i < 8; ++i) {
            bytes[kAt + 7 - i] = static_cast<char>(packed & 0xFFU);
            packed >>= 8U;
        }
    });
}

/// A source that reads through `inner` until a read would reach byte
/// `fail_at`, and fails every such read with EIO.
class FailingSource final : public Source {
public:
    FailingSource(std::unique_ptr<Source> inner, std::uint64_t fail_at) noexcept
        : inner_{std::move(inner)}, fail_at_{fail_at} {}

    std::expected<std::size_t, int> read_at(std::uint64_t offset,
                                            std::span<std::byte> buffer) override {
        if (offset + buffer.size() > fail_at_) {
            return std::unexpected{EIO};
        }
        return inner_->read_at(offset, buffer);
    }

    std::expected<std::uint64_t, int> size() override { return inner_->size(); }

private:
    std::unique_ptr<Source> inner_;
    std::uint64_t fail_at_;
};

/// A runtime directory, a factory, and a segment from it, mapped read-write.
struct Segment {
    ipc::test::TempDir dir;
    std::unique_ptr<ipc::SegmentFactory> factory;
    ipc::Fd fd;
    std::unique_ptr<ipc::Region> region;
};

/// A new segment of `kind` and `size`, mapped read-write.
inline std::unique_ptr<Segment> make_segment(ipc::SegmentKind kind, std::uint32_t layout_version,
                                             std::size_t size) {
    auto segment = std::make_unique<Segment>();
    REQUIRE(!segment->dir.path().empty());
    std::expected<ipc::SegmentFactory, std::error_code> factory =
        ipc::SegmentFactory::open(segment->dir.path());
    REQUIRE(factory.has_value());
    segment->factory = std::make_unique<ipc::SegmentFactory>(std::move(*factory));
    std::expected<ipc::Fd, std::error_code> fd =
        segment->factory->create(kind, layout_version, size);
    REQUIRE(fd.has_value());
    segment->fd = std::move(*fd);
    std::expected<ipc::Region, ipc::MapFailure> region =
        ipc::Region::map(segment->fd.get(), kind, layout_version, ipc::Access::ReadWrite);
    REQUIRE(region.has_value());
    segment->region = std::make_unique<ipc::Region>(std::move(*region));
    return segment;
}

/// A new track segment of `size` bytes, mapped read-write.
inline std::unique_ptr<Segment> make_track_segment(std::size_t size = ipc::kTrackSegmentSize) {
    return make_segment(ipc::SegmentKind::Track, ipc::kTrackLayoutVersion, size);
}

/// The track header in `segment`.
inline ipc::TrackHeader& header_of(const Segment& segment) {
    auto* const header = segment.region->at<ipc::TrackHeader>(ipc::kSegmentHeaderSize);
    REQUIRE(header != nullptr);
    return *header;
}

/// The frames `segment` holds.
inline std::uint64_t capacity_of(const Segment& segment) {
    return (segment.region->bytes().size() - ipc::kTrackSamplesOffset) / ipc::kTrackFrameSize;
}

/// The sample area of `segment`, as the worker writes it.
inline std::span<const float> samples_of(const Segment& segment) {
    const std::span<std::byte> bytes = segment.region->bytes().subspan(ipc::kTrackSamplesOffset);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sample area holds floats
    return {reinterpret_cast<const float*>(bytes.data()), bytes.size() / sizeof(float)};
}

/// The track's progress, or nothing if the header was refused.
inline std::optional<ipc::TrackProgress> progress_of(const Segment& segment) {
    const std::expected<ipc::TrackProgress, ipc::TrackHeaderError> progress =
        ipc::read_track(header_of(segment), capacity_of(segment));
    if (!progress) {
        return std::nullopt;
    }
    return *progress;
}

/// The track's progress, which the header must allow.
inline ipc::TrackProgress required_progress(const Segment& segment) {
    const std::optional<ipc::TrackProgress> progress = progress_of(segment);
    REQUIRE(progress.has_value());
    return progress.value_or(ipc::TrackProgress{});
}

/// True if every byte of the 2,048 frames from frame `frame` on (or up to the
/// end) is zero: nothing was written past the frames before it.
inline bool zero_after(const Segment& segment, std::uint64_t frame) {
    const std::span<const float> samples = samples_of(segment);
    const std::size_t from = static_cast<std::size_t>(frame) * 2;
    const std::size_t to = std::min(samples.size(), from + 4'096);
    return std::all_of(samples.begin() + static_cast<std::ptrdiff_t>(from),
                       samples.begin() + static_cast<std::ptrdiff_t>(to), [](float sample) {
                           std::uint32_t bits = 0;
                           std::memcpy(&bits, &sample, sizeof bits);
                           return bits == 0;
                       });
}

} // namespace ks::decode::test
