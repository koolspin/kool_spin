// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the safety-stage acceptance tests: a graph that writes a function
// of the frame and channel, and rendering through the offline driver.

#include <ks/core/realtime.h>
#include <ks/engine/block.h>
#include <ks/engine/offline.h>
#include <ks/engine/render_core.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ks::engine::test {

inline constexpr std::uint32_t kRate = 48'000;
/// The default 10 ms fade at kRate.
inline constexpr std::size_t kFadeFrames = 480;
/// Absolute tolerance for float comparisons.
inline constexpr double kTolerance = 1e-6;

/// The default −1 dBFS ceiling, as a gain.
inline double default_ceiling() {
    return std::pow(10.0, -1.0 / 20.0);
}

/// Writes value(frame, channel), counting frames from the first block it
/// renders.
template <typename Value>
class FunctionGraph final : public Graph {
public:
    explicit FunctionGraph(Value value) : value_{value} {}

    void process(const Block& block) noexcept KS_NONBLOCKING override {
        for (std::size_t c = 0; c < block.channels.size(); ++c) {
            const std::span<float> channel = block.channels[c];
            for (std::size_t i = 0; i < block.frames; ++i) {
                channel[i] = value_(next_frame_ + i, c);
            }
        }
        next_frame_ += block.frames;
    }

private:
    Value value_;
    std::uint64_t next_frame_ = 0;
};

/// Renders `frames` frames of `channels` channels through `core` with the
/// offline driver, as `driver`, and returns them, one vector per channel.
inline std::vector<std::vector<float>> render(RenderCore& core, std::size_t frames,
                                              DriverId driver = kDefaultDriver,
                                              std::size_t block_frames = 64,
                                              std::size_t channels = 2) {
    std::vector<std::vector<float>> output(channels, std::vector<float>(frames, 5.0F));
    std::vector<std::span<float>> views(output.begin(), output.end());
    const OfflineConfig config{
        .sample_rate = kRate, .block_frames = block_frames, .start_time = {}, .driver = driver};
    static_cast<void>(render_offline(core, config, views));
    return output;
}

/// True if `actual` is within kTolerance of `expected`.
inline bool near(float actual, double expected) {
    return std::abs(static_cast<double>(actual) - expected) <= kTolerance;
}

} // namespace ks::engine::test
