// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/render-core/spec.md: the miniaudio backend, and
// interleaving.

#include <ks/engine/miniaudio_backend.h>

#include <ks/core/realtime.h>
#include <ks/engine/audio_backend.h>
#include <ks/engine/block.h>
#include <ks/engine/interleave.h>
#include <ks/engine/render_core.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iostream>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using ks::engine::AudioBackend;
using ks::engine::BackendError;
using ks::engine::Block;
using ks::engine::Graph;
using ks::engine::interleave;
using ks::engine::kMaxBlockFrames;
using ks::engine::kMaxOutputChannels;
using ks::engine::MiniaudioBackends;
using ks::engine::MiniaudioConfig;
using ks::engine::open_miniaudio;
using ks::engine::RenderCore;
using ks::engine::StreamInfo;
using ks::engine::test::BlockRecord;
using ks::engine::test::Planar;
using ks::engine::test::signal;
using ks::engine::test::SignalGraph;
using ks::engine::test::SilentGraph;
using ks::engine::test::StallWatch;

using Clock = std::chrono::steady_clock;
using Opened = std::expected<std::unique_ptr<AudioBackend>, BackendError>;

/// Waits until the core has rendered at least `target` frames. Returns false
/// if rendering stops making progress first.
bool wait_for_frames(const RenderCore& core, std::uint64_t target) {
    StallWatch watch;
    std::uint64_t last = core.frames_rendered();
    while (last < target) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        const std::uint64_t now = core.frames_rendered();
        if (now != last) {
            last = now;
            watch.progressed();
        } else if (watch.stalled()) {
            return false;
        }
    }
    return true;
}

std::string error_text(const Opened& opened) {
    return opened.has_value() ? std::string{} : opened.error().message;
}

/// A sine wave on every channel, at the device's rate. It starts at zero
/// phase, and fades out over 10 ms once asked, so starting and stopping don't
/// click.
class ToneGraph final : public Graph {
public:
    ToneGraph(double frequency_hz, float amplitude) noexcept
        : frequency_hz_{frequency_hz}, amplitude_{amplitude} {}

    void fade_out() noexcept { fading_.store(true, std::memory_order_relaxed); }

    void process(const Block& block) noexcept KS_NONBLOCKING override {
        const double increment =
            2.0 * std::numbers::pi * frequency_hz_ / static_cast<double>(block.sample_rate);
        const bool fading = fading_.load(std::memory_order_relaxed);
        const float fade_step = 100.0F / static_cast<float>(block.sample_rate); // 10 ms
        for (std::size_t i = 0; i < block.frames; ++i) {
            if (fading) {
                gain_ = std::max(0.0F, gain_ - fade_step);
            }
            const float sample = gain_ * amplitude_ * static_cast<float>(std::sin(phase_));
            for (const std::span<float> channel : block.channels) {
                channel[i] = sample;
            }
            phase_ += increment;
            if (phase_ >= 2.0 * std::numbers::pi) {
                phase_ -= 2.0 * std::numbers::pi;
            }
        }
    }

private:
    double frequency_hz_;
    float amplitude_;
    float gain_ = 1.0F;
    double phase_ = 0.0;
    std::atomic<bool> fading_{false};
};

} // namespace

TEST_CASE("Miniaudio: renders through the null backend", "[miniaudio]") {
    constexpr std::uint32_t kRate = 48'000;
    constexpr std::uint32_t kChannels = 2;
    constexpr std::uint64_t kTenthOfASecond = kRate / 10;

    SignalGraph graph{100'000};
    RenderCore core{graph};
    const Opened opened = open_miniaudio(core, MiniaudioConfig{
                                                   .backends = MiniaudioBackends::Null,
                                                   .channels = kChannels,
                                                   .sample_rate = kRate,
                                                   .period_frames = 0,
                                               });
    CAPTURE(error_text(opened));
    REQUIRE(opened.has_value());
    AudioBackend& backend = **opened;

    const StreamInfo& info = backend.info();
    REQUIRE(info.sample_rate == kRate);
    REQUIRE(info.channels == kChannels);

    const Clock::time_point before_start = Clock::now();
    REQUIRE(backend.start().has_value());
    REQUIRE(wait_for_frames(core, kTenthOfASecond));
    backend.stop();
    const Clock::time_point after_stop = Clock::now();

    // Nothing renders once stop() has returned.
    const std::size_t calls_at_stop = graph.calls();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    REQUIRE(graph.calls() == calls_at_stop);

    const std::span<const BlockRecord> records = graph.records();
    REQUIRE_FALSE(records.empty());
    for (const BlockRecord& record : records) {
        REQUIRE(record.frames >= 1);
        REQUIRE(record.frames <= kMaxBlockFrames);
        REQUIRE(record.channels == kChannels);
        REQUIRE(record.sample_rate == kRate);
        REQUIRE(record.host_time >= before_start);
        REQUIRE(record.host_time <= after_stop + std::chrono::seconds{1});
    }

    // Starting again resumes rendering.
    const std::uint64_t frames_at_stop = core.frames_rendered();
    REQUIRE(backend.start().has_value());
    REQUIRE(wait_for_frames(core, frames_at_stop + kTenthOfASecond));
    backend.stop();
}

TEST_CASE("Miniaudio: refuses more channels than the engine renders", "[miniaudio]") {
    SilentGraph graph;
    RenderCore core{graph};
    const Opened opened = open_miniaudio(core, MiniaudioConfig{
                                                   .backends = MiniaudioBackends::Null,
                                                   .channels = kMaxOutputChannels + 1,
                                                   .sample_rate = 48'000,
                                                   .period_frames = 0,
                                               });
    REQUIRE_FALSE(opened.has_value());
    CAPTURE(opened.error().message);
    REQUIRE(opened.error().kind == BackendError::Kind::TooManyChannels);
    REQUIRE(graph.calls() == 0);
}

TEST_CASE("Interleave: samples land in place", "[interleave]") {
    constexpr std::size_t kFrames = 100;

    for (const std::size_t channels : std::array<std::size_t, 3>{1, 2, 8}) {
        CAPTURE(channels);
        const Planar planar{channels, kFrames, 0.0F};
        for (std::size_t c = 0; c < channels; ++c) {
            const std::span<float> channel = planar.channels()[c];
            for (std::size_t f = 0; f < kFrames; ++f) {
                channel[f] = signal(f, c);
            }
        }

        // All the frames.
        std::vector<float> interleaved(kFrames * channels, 5.0F);
        REQUIRE(interleave(planar.channels(), kFrames, interleaved));
        for (std::size_t f = 0; f < kFrames; ++f) {
            for (std::size_t c = 0; c < channels; ++c) {
                REQUIRE(interleaved.at((f * channels) + c) == signal(f, c));
            }
        }

        // Only the first `frames`; the rest of the buffer is untouched.
        constexpr std::size_t kSome = 60;
        std::vector<float> partial(kFrames * channels, 5.0F);
        REQUIRE(interleave(planar.channels(), kSome, partial));
        for (std::size_t f = 0; f < kFrames; ++f) {
            for (std::size_t c = 0; c < channels; ++c) {
                const float expected = f < kSome ? signal(f, c) : 5.0F;
                REQUIRE(partial.at((f * channels) + c) == expected);
            }
        }

        // An interleaved buffer one sample short: nothing is written.
        std::vector<float> short_buffer((kFrames * channels) - 1, 5.0F);
        REQUIRE_FALSE(interleave(planar.channels(), kFrames, short_buffer));
        REQUIRE(std::ranges::all_of(short_buffer, [](float sample) { return sample == 5.0F; }));
    }

    // A channel shorter than the frame count: nothing is written.
    const Planar ragged{{kFrames, kFrames - 1}, 1.0F};
    std::vector<float> interleaved(kFrames * 2, 5.0F);
    REQUIRE_FALSE(interleave(ragged.channels(), kFrames, interleaved));
    REQUIRE(std::ranges::all_of(interleaved, [](float sample) { return sample == 5.0F; }));
}

// For the [manual] criterion. Hidden, so it runs only when asked for:
//   ks_render_core_acceptance_tests "[listen]"
TEST_CASE("Listen: a tone through the default device", "[.][listen]") {
    constexpr double kFrequencyHz = 440.0;
    constexpr float kAmplitude = 0.1F; // -20 dBFS
    constexpr std::chrono::seconds kDuration{3};

    ToneGraph graph{kFrequencyHz, kAmplitude};
    RenderCore core{graph};
    const Opened opened = open_miniaudio(core, MiniaudioConfig{
                                                   .backends = MiniaudioBackends::System,
                                                   .channels = 0,
                                                   .sample_rate = 0,
                                                   .period_frames = 128,
                                               });
    CAPTURE(error_text(opened));
    REQUIRE(opened.has_value());
    AudioBackend& backend = **opened;

    const StreamInfo& info = backend.info();
    std::cout << std::format("Playing {} Hz at -20 dBFS for {} on {}: {}, {} Hz, {} channels, "
                             "{}-frame period\n",
                             kFrequencyHz, kDuration, info.backend, info.device, info.sample_rate,
                             info.channels, info.period_frames)
              << std::flush;

    REQUIRE(backend.start().has_value());
    std::this_thread::sleep_for(kDuration);
    graph.fade_out();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    backend.stop();

    const auto expected = static_cast<std::uint64_t>(info.sample_rate) *
                          static_cast<std::uint64_t>(kDuration.count());
    std::cout << std::format("Rendered {} frames ({} expected)\n", core.frames_rendered(),
                             expected);
    REQUIRE(core.frames_rendered() >= expected * 9 / 10);
}
