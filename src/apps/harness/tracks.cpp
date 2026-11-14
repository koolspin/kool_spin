// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/harness/tracks.h>

#include <ks/engine/track.h>
#include <ks/harness/script.h>

#include <ks_codecs.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ks::harness {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

/// The noise generator's seed: any fixed value would do.
constexpr std::uint64_t kNoiseSeed = 0x6b73'5f68'6172'6e65ULL;

/// A click: 10 ms of a 1 kHz sine under a Hann window.
constexpr double kClickSeconds = 0.010;
constexpr double kClickHz = 1'000.0;

/// The peak amplitude, −12 dBFS as a linear gain.
double peak_gain() {
    return std::pow(10.0, kSignalPeakDbfs / 20.0);
}

/// SplitMix64: small, and the same on every platform.
class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) : state_{seed} {}

    std::uint64_t next() {
        state_ += 0x9e37'79b9'7f4a'7c15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d0'49bb'1331'11ebULL;
        return z ^ (z >> 31U);
    }

    /// Uniform in [−1, 1): the top 24 bits, centred and scaled by a power
    /// of two, so the result is exact.
    double next_bipolar() {
        constexpr std::int64_t kHalf = std::int64_t{1} << 23U;
        const auto bits = static_cast<std::int64_t>(next() >> 40U);
        return static_cast<double>(bits - kHalf) / static_cast<double>(kHalf);
    }

private:
    std::uint64_t state_;
};

void fill_sine(std::span<double> mono, double hz, double rate) {
    std::size_t n = 0;
    for (double& sample : mono) {
        sample = std::sin(kTwoPi * hz * static_cast<double>(n) / rate);
        ++n;
    }
}

/// Frequency from × r^(t / L), r = to / from; its phase, integrated in closed
/// form, is 2π·from·L/ln(r)·(r^(t/L) − 1).
void fill_sweep(std::span<double> mono, double from, double to, double seconds, double rate) {
    const double log_ratio = std::log(to / from);
    std::size_t n = 0;
    for (double& sample : mono) {
        const double t = static_cast<double>(n) / rate;
        const double phase = log_ratio == 0.0 ? kTwoPi * from * t
                                              : kTwoPi * from * seconds / log_ratio *
                                                    std::expm1(log_ratio * t / seconds);
        sample = std::sin(phase);
        ++n;
    }
}

void fill_noise(std::span<double> mono) {
    SplitMix64 generator{kNoiseSeed};
    for (double& sample : mono) {
        sample = generator.next_bipolar();
    }
}

/// A click at each beat from 0, silence between. `mono` starts silent.
void fill_clicks(std::span<double> mono, double bpm, double rate) {
    const auto burst = static_cast<std::size_t>(std::llround(kClickSeconds * rate));
    // The reader allows only positive tempos; anything else, NaN included,
    // gives silence rather than an endless loop.
    if (!(bpm > 0.0) || burst == 0) {
        return;
    }
    const double beat_frames = 60.0 / bpm * rate;
    for (std::uint64_t beat = 0;; ++beat) {
        const auto start =
            static_cast<std::size_t>(std::llround(static_cast<double>(beat) * beat_frames));
        if (start >= mono.size()) {
            break;
        }
        const std::span<double> click = mono.subspan(start, std::min(burst, mono.size() - start));
        std::size_t n = 0;
        for (double& sample : click) {
            const auto position = static_cast<double>(n);
            const double window =
                0.5 * (1.0 - std::cos(kTwoPi * position / static_cast<double>(burst)));
            sample = window * std::sin(kTwoPi * kClickHz * position / rate);
            ++n;
        }
    }
}

} // namespace

std::vector<float> generate(const Signal& signal) {
    const auto rate = static_cast<double>(signal.sample_rate);
    const auto frames = static_cast<std::size_t>(std::llround(signal.seconds * rate));
    std::vector<double> mono(frames, 0.0);
    switch (signal.kind) {
    case SignalKind::Sine:
        fill_sine(mono, signal.frequency_hz, rate);
        break;
    case SignalKind::Sweep:
        fill_sweep(mono, signal.frequency_hz, signal.to_hz, signal.seconds, rate);
        break;
    case SignalKind::Noise:
        fill_noise(mono);
        break;
    case SignalKind::Clicks:
        fill_clicks(mono, signal.bpm, rate);
        break;
    }

    const double gain = peak_gain();
    std::vector<float> samples;
    samples.reserve(2 * frames);
    for (const double sample : mono) {
        const auto value = static_cast<float>(sample * gain);
        samples.push_back(value);
        samples.push_back(value);
    }
    return samples;
}

std::expected<DecodedAudio, std::string> decode_file(const std::filesystem::path& path) {
    const std::string name = path.string();
    ks_codecs_audio audio{};
    const int result = ks_codecs_decode_file(name.c_str(), engine::kMaxTrackFrames, &audio);
    if (result != KS_CODECS_OK) {
        return std::unexpected{name + ": " + ks_codecs_describe(result)};
    }

    // Copy the samples out and free the codec's buffer before checking
    // anything, so no path leaks it.
    DecodedAudio decoded;
    decoded.sample_rate = audio.sample_rate;
    if (audio.samples != nullptr) {
        const std::span<const float> samples{audio.samples,
                                             static_cast<std::size_t>(audio.frames * 2U)};
        decoded.samples.assign(samples.begin(), samples.end());
    }
    ks_codecs_free_audio(&audio);

    if (decoded.sample_rate < engine::kMinTrackRate ||
        decoded.sample_rate > engine::kMaxTrackRate) {
        return std::unexpected{name + ": the sample rate is " +
                               std::to_string(decoded.sample_rate) + " Hz; a track's must be " +
                               std::to_string(engine::kMinTrackRate) + " to " +
                               std::to_string(engine::kMaxTrackRate) + " Hz"};
    }
    return decoded;
}

std::expected<TrackSet, std::string> TrackSet::prepare(const Script& script) {
    TrackSet set;
    set.samples_.reserve(script.tracks.size());
    set.tracks_.reserve(script.tracks.size());
    for (const TrackSpec& spec : script.tracks) {
        std::uint32_t rate = 0;
        std::vector<float> samples;
        if (const auto* file = std::get_if<TrackFile>(&spec.source)) {
            std::expected<DecodedAudio, std::string> decoded = decode_file(file->path);
            if (!decoded.has_value()) {
                return std::unexpected{std::move(decoded.error())};
            }
            rate = decoded->sample_rate;
            samples = std::move(decoded->samples);
        } else {
            const auto& signal = std::get<Signal>(spec.source);
            rate = signal.sample_rate;
            samples = generate(signal);
        }

        set.samples_.push_back(std::move(samples));
        const std::uint64_t id = set.tracks_.size() + 1;
        auto track = std::make_unique<engine::Track>(set.samples_.back(), rate, id);
        track->publish(track->capacity(), true);
        set.tracks_.push_back(std::move(track));
    }
    return set;
}

std::size_t TrackSet::size() const noexcept {
    return tracks_.size();
}

const engine::Track& TrackSet::track(std::size_t index) const {
    return *tracks_.at(index);
}

} // namespace ks::harness
