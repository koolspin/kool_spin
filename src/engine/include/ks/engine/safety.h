// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The output safety stage (ADR 0002 §5): the last stage before the device,
/// always on. The render core runs it after the graph on every rendered
/// block, so no driver can bypass it.

#include <ks/core/realtime.h>
#include <ks/engine/block.h>

#include <chrono>
#include <cstdint>

namespace ks::engine {

struct SafetyConfig {
    /// How long a fade in or out takes. 0: changes are immediate.
    std::chrono::milliseconds fade{10};
    /// The most the output may reach. Above 0 is treated as 0.
    double ceiling_dbfs = -1.0;
    /// The limiter's release time constant.
    std::chrono::milliseconds release{100};
};

/// The stage's state, carried from one block to the next. The render core
/// owns one; only the render token's holder uses it.
class SafetyStage {
public:
    explicit SafetyStage(const SafetyConfig& config) noexcept;

    /// Runs over the block's frames, in place: NaN and Inf to silence, the
    /// fade, the limiter, and the clamp. `muted` sets the fade's target; a
    /// block marked resumed restarts the fade from silence.
    void process(const Block& block, bool muted) noexcept KS_NONBLOCKING;

    /// True when muted and the fade gain has reached 0.
    [[nodiscard]] bool silenced() const noexcept KS_NONBLOCKING;

    /// NaN and infinite samples silenced so far.
    [[nodiscard]] std::uint64_t non_finite_samples() const noexcept KS_NONBLOCKING;

    /// Frames in which the limiter's gain was below 1 so far.
    [[nodiscard]] std::uint64_t limited_frames() const noexcept KS_NONBLOCKING;

private:
    float ceiling_;
    std::chrono::nanoseconds fade_;
    std::chrono::nanoseconds release_;

    /// The sample rate the per-rate values below are for.
    std::uint32_t sample_rate_ = 0;
    /// The fade's length in frames, n.
    std::uint64_t fade_frames_ = 0;
    /// 1 − e^(−1 / (release × sample rate)).
    float release_step_ = 0.0F;

    /// The fade position, p: the fade gain is p / n.
    std::uint64_t fade_position_ = 0;
    bool muted_ = false;
    float limiter_gain_ = 1.0F;

    std::uint64_t non_finite_samples_ = 0;
    std::uint64_t limited_frames_ = 0;
};

} // namespace ks::engine
