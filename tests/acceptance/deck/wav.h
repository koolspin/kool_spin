// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Reading and writing WAV files, for the deck's golden file and its listening
// test. Test code only: the engine never reads files.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ks::engine::test {

static_assert(std::endian::native == std::endian::little, "WAV files are little-endian");

/// Audio read from a WAV file, as a track holds it.
struct WavAudio {
    std::uint32_t sample_rate = 0;
    /// Interleaved stereo. Mono is copied to both channels; files with more
    /// channels keep the first two.
    std::vector<float> samples;
};

namespace wav_detail {

inline std::uint32_t read_u32(std::span<const char> bytes, std::size_t at) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.subspan(at, sizeof value).data(), sizeof value);
    return value;
}

inline std::uint16_t read_u16(std::span<const char> bytes, std::size_t at) {
    std::uint16_t value = 0;
    std::memcpy(&value, bytes.subspan(at, sizeof value).data(), sizeof value);
    return value;
}

inline bool tag_is(std::span<const char> bytes, std::size_t at, std::string_view tag) {
    return bytes.size() >= at + tag.size() &&
           std::string_view{bytes.subspan(at, tag.size()).data(), tag.size()} == tag;
}

/// One sample, as a float in [-1, 1].
inline float decode_sample(std::span<const char> bytes, std::uint16_t format, std::uint16_t bits) {
    constexpr std::uint16_t kFloat = 3;
    if (format == kFloat) {
        float value = 0.0F;
        std::memcpy(&value, bytes.data(), sizeof value);
        return value;
    }
    // Integer PCM: sign-extend from the top byte.
    std::int32_t value = 0;
    const std::size_t width = bits / 8U;
    for (std::size_t i = 0; i < width; ++i) {
        value |= static_cast<std::int32_t>(static_cast<std::uint8_t>(bytes[i])) << (8U * i);
    }
    const unsigned shift = 32U - bits;
    value = static_cast<std::int32_t>(static_cast<std::uint32_t>(value) << shift) >> shift;
    return static_cast<float>(static_cast<double>(value) / static_cast<double>(1U << (bits - 1U)));
}

inline void append_u32(std::vector<char>& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((value >> (8U * i)) & 0xFFU));
    }
}

inline void append_u16(std::vector<char>& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

inline void append_tag(std::vector<char>& out, std::string_view tag) {
    out.insert(out.end(), tag.begin(), tag.end());
}

} // namespace wav_detail

/// Reads a WAV file of 16-, 24-, or 32-bit integer PCM, or 32-bit float, in
/// the plain or the extensible format. Returns nothing for anything else.
inline std::optional<WavAudio> read_wav(const std::filesystem::path& path) {
    using namespace wav_detail;
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return std::nullopt;
    }
    const std::vector<char> raw{std::istreambuf_iterator<char>{file},
                                std::istreambuf_iterator<char>{}};
    const std::span<const char> bytes{raw};
    if (!tag_is(bytes, 0, "RIFF") || !tag_is(bytes, 8, "WAVE")) {
        return std::nullopt;
    }

    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t bits = 0;
    std::span<const char> data;
    for (std::size_t at = 12; at + 8 <= bytes.size();) {
        const std::uint32_t size = read_u32(bytes, at + 4);
        if (size > bytes.size() - at - 8) {
            return std::nullopt;
        }
        const std::span<const char> body = bytes.subspan(at + 8, size);
        if (tag_is(bytes, at, "fmt ") && size >= 16) {
            format = read_u16(body, 0);
            channels = read_u16(body, 2);
            sample_rate = read_u32(body, 4);
            bits = read_u16(body, 14);
            constexpr std::uint16_t kExtensible = 0xFFFE;
            if (format == kExtensible && size >= 26) {
                format = read_u16(body, 24); // the subformat GUID's first two bytes
            }
        } else if (tag_is(bytes, at, "data")) {
            data = body;
        }
        at += 8 + size + (size % 2);
    }

    const bool pcm = format == 1 && (bits == 16 || bits == 24 || bits == 32);
    const bool floating = format == 3 && bits == 32;
    if ((!pcm && !floating) || channels == 0 || sample_rate == 0) {
        return std::nullopt;
    }
    const std::size_t sample_bytes = bits / 8U;
    const std::size_t frame_bytes = sample_bytes * channels;
    const std::size_t frames = data.size() / frame_bytes;

    WavAudio audio{.sample_rate = sample_rate, .samples = {}};
    audio.samples.reserve(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::span<const char> bytes_of_frame = data.subspan(frame * frame_bytes, frame_bytes);
        const float left = decode_sample(bytes_of_frame, format, bits);
        const float right =
            channels > 1 ? decode_sample(bytes_of_frame.subspan(sample_bytes), format, bits) : left;
        audio.samples.push_back(left);
        audio.samples.push_back(right);
    }
    return audio;
}

/// Writes 32-bit float stereo. Returns false if the file can't be written.
inline bool write_wav(const std::filesystem::path& path, std::uint32_t sample_rate,
                      std::span<const float> left, std::span<const float> right) {
    using namespace wav_detail;
    const std::size_t frames = std::min(left.size(), right.size());
    const auto data_bytes = static_cast<std::uint32_t>(frames * 2 * sizeof(float));

    std::vector<char> out;
    out.reserve(58 + data_bytes);
    append_tag(out, "RIFF");
    append_u32(out, 50 + data_bytes);
    append_tag(out, "WAVE");
    append_tag(out, "fmt ");
    append_u32(out, 18);
    append_u16(out, 3); // IEEE float
    append_u16(out, 2);
    append_u32(out, sample_rate);
    append_u32(out, sample_rate * 2 * static_cast<std::uint32_t>(sizeof(float)));
    append_u16(out, 2 * sizeof(float));
    append_u16(out, 32);
    append_u16(out, 0); // no extension
    append_tag(out, "fact");
    append_u32(out, 4);
    append_u32(out, static_cast<std::uint32_t>(frames));
    append_tag(out, "data");
    append_u32(out, data_bytes);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        append_u32(out, std::bit_cast<std::uint32_t>(left[frame]));
        append_u32(out, std::bit_cast<std::uint32_t>(right[frame]));
    }

    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(file);
}

} // namespace ks::engine::test
