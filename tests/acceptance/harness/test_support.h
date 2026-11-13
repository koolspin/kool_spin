// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the harness acceptance tests: reading the WAV files the harness
// writes, measuring signals, temporary files, and error text.

#include <ks/harness/script.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ks::harness::test {

static_assert(std::endian::native == std::endian::little, "WAV files are little-endian");

/// Where the audio fixtures are.
inline std::filesystem::path data_dir() {
    return std::filesystem::path{KS_HARNESS_TEST_DATA};
}

/// The signals' peak, −12 dBFS, as a linear gain.
inline const double kPeak = std::pow(10.0, -12.0 / 20.0);

/// 20 log10 |x|.
inline double dbfs(double x) {
    return 20.0 * std::log10(std::abs(x));
}

/// The largest |sample|.
inline double peak(std::span<const float> samples) {
    double largest = 0.0;
    for (const float sample : samples) {
        largest = std::max(largest, static_cast<double>(std::abs(sample)));
    }
    return largest;
}

/// One channel of interleaved samples.
inline std::vector<float> channel(std::span<const float> interleaved, std::size_t channels,
                                  std::size_t which) {
    std::vector<float> out;
    out.reserve(interleaved.size() / channels);
    for (std::size_t i = which; i < interleaved.size(); i += channels) {
        out.push_back(interleaved[i]);
    }
    return out;
}

/// A frequency measured over a stretch of a signal.
struct Measured {
    /// 0 if there were fewer than two rising zero crossings.
    double hz = 0.0;
    /// The middle of the first and last crossings, in seconds.
    double at_seconds = 0.0;
};

/// The frequency of a periodic signal over [first, last) frames, from its
/// rising zero crossings, placed between samples by linear interpolation.
inline Measured measure(std::span<const float> samples, std::uint32_t sample_rate,
                        std::size_t first, std::size_t last) {
    last = std::min(last, samples.size());
    std::optional<double> first_crossing;
    double last_crossing = 0.0;
    std::size_t crossings = 0;
    for (std::size_t i = first + 1; i < last; ++i) {
        const auto before = static_cast<double>(samples[i - 1]);
        const auto after = static_cast<double>(samples[i]);
        if (before < 0.0 && after >= 0.0) {
            const double at = static_cast<double>(i - 1) + (-before / (after - before));
            if (!first_crossing.has_value()) {
                first_crossing = at;
            }
            last_crossing = at;
            ++crossings;
        }
    }
    if (crossings < 2) {
        return {};
    }
    const double start = first_crossing.value_or(0.0);
    const auto cycles = static_cast<double>(crossings - 1);
    return Measured{.hz = cycles * sample_rate / (last_crossing - start),
                    .at_seconds = (start + last_crossing) / 2.0 / sample_rate};
}

/// measure()'s frequency.
inline double frequency(std::span<const float> samples, std::uint32_t sample_rate,
                        std::size_t first, std::size_t last) {
    return measure(samples, sample_rate, first, last).hz;
}

/// Whether `actual` is within `fraction` of `expected`.
inline bool within(double actual, double expected, double fraction) {
    return std::abs(actual - expected) <= std::abs(expected) * fraction;
}

/// A WAV file of 32-bit floats, as the harness writes them.
struct FloatWav {
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    /// Interleaved.
    std::vector<float> samples;

    [[nodiscard]] std::size_t frames() const {
        return channels == 0 ? 0 : samples.size() / channels;
    }
};

inline std::vector<char> read_bytes(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::vector<char>{std::istreambuf_iterator<char>{file},
                             std::istreambuf_iterator<char>{}};
}

namespace wav_detail {

template <typename T>
T read_le(std::span<const char> bytes, std::size_t at) {
    T value{};
    std::memcpy(&value, bytes.subspan(at, sizeof value).data(), sizeof value);
    return value;
}

inline bool tag_is(std::span<const char> bytes, std::size_t at, std::string_view tag) {
    return bytes.size() >= at + tag.size() &&
           std::string_view{bytes.subspan(at, tag.size()).data(), tag.size()} == tag;
}

} // namespace wav_detail

/// Reads a 32-bit float WAV file, plain or extensible, with any number of
/// channels. Nothing for anything else, or a malformed file.
inline std::optional<FloatWav> read_float_wav(const std::filesystem::path& path) {
    using wav_detail::read_le;
    using wav_detail::tag_is;
    constexpr std::uint16_t kFloat = 3;
    constexpr std::uint16_t kExtensible = 0xFFFE;

    const std::vector<char> raw = read_bytes(path);
    const std::span<const char> bytes{raw};
    if (!tag_is(bytes, 0, "RIFF") || !tag_is(bytes, 8, "WAVE")) {
        return std::nullopt;
    }
    FloatWav wav;
    bool have_format = false;
    std::size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const auto size = read_le<std::uint32_t>(bytes, at + 4);
        const std::size_t body = at + 8;
        if (body + size > bytes.size()) {
            return std::nullopt;
        }
        if (tag_is(bytes, at, "fmt ")) {
            if (size < 16) {
                return std::nullopt;
            }
            auto format = read_le<std::uint16_t>(bytes, body);
            wav.channels = read_le<std::uint16_t>(bytes, body + 2);
            wav.sample_rate = read_le<std::uint32_t>(bytes, body + 4);
            const auto bits = read_le<std::uint16_t>(bytes, body + 14);
            if (format == kExtensible && size >= 26) {
                format = read_le<std::uint16_t>(bytes, body + 24); // the subformat GUID's start
            }
            if (format != kFloat || bits != 32 || wav.channels == 0) {
                return std::nullopt;
            }
            have_format = true;
        } else if (tag_is(bytes, at, "data")) {
            if (!have_format) {
                return std::nullopt;
            }
            wav.samples.resize(size / sizeof(float));
            std::memcpy(wav.samples.data(), bytes.subspan(body).data(),
                        wav.samples.size() * sizeof(float));
            return wav;
        }
        at = body + size + (size % 2); // chunks are padded to an even size
    }
    return std::nullopt;
}

/// A new, empty directory for one test's files.
inline std::filesystem::path temp_dir(std::string_view name) {
    std::random_device random;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        (std::string{"ks-harness-"} + std::string{name} + "-" + std::to_string(random()));
    std::filesystem::create_directories(dir);
    return dir;
}

/// The error's text, for CAPTURE; empty for a value.
template <typename T>
std::string error_text(const std::expected<T, std::string>& result) {
    return result.has_value() ? std::string{} : result.error();
}

template <typename T>
std::string error_text(const std::expected<T, ScriptError>& result) {
    return result.has_value() ? std::string{}
                              : std::to_string(result.error().line) + ": " + result.error().message;
}

/// Joins lines into a script, each ending in a newline.
inline std::string lines(std::span<const std::string_view> each) {
    std::string text;
    for (const std::string_view line : each) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

} // namespace ks::harness::test
