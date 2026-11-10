// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/safety.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ks::engine {

namespace {

/// The limiter's gain becomes 1 once it's releasing to 1 and this close.
constexpr double kReleaseSnap = 1e-4;

/// Longer fades are treated as this long, so frame counts can't overflow.
constexpr std::chrono::milliseconds kMaxFade = std::chrono::hours{24};

/// The ceiling as a gain. Above 0 dBFS, or NaN, is treated as 0 dBFS.
float ceiling_gain(double dbfs) noexcept {
    if (!(dbfs < 0.0)) {
        return 1.0F;
    }
    return static_cast<float>(std::pow(10.0, dbfs / 20.0));
}

/// `duration` in frames at `sample_rate`, rounded to the nearest frame. A
/// negative duration is 0.
std::uint64_t to_frames(std::chrono::milliseconds duration,
                        std::uint32_t sample_rate) noexcept KS_NONBLOCKING {
    const auto ms = static_cast<std::uint64_t>(
        std::clamp(duration, std::chrono::milliseconds{0}, kMaxFade).count());
    return ((ms * sample_rate) + 500) / 1000;
}

/// 1 − e^(−1 / (release × sample_rate)): 1, an instant release, if release
/// isn't positive.
double release_step(std::chrono::milliseconds release,
                    std::uint32_t sample_rate) noexcept KS_NONBLOCKING {
    if (release <= std::chrono::milliseconds{0} || sample_rate == 0) {
        return 1.0;
    }
    const double frames =
        std::chrono::duration<double>(release).count() * static_cast<double>(sample_rate);
    return -std::expm1(-1.0 / frames);
}

} // namespace

SafetyStage::SafetyStage(const SafetyConfig& config) noexcept
    : config_{config}, ceiling_{ceiling_gain(config.ceiling_dbfs)} {}

void SafetyStage::set_sample_rate(std::uint32_t sample_rate) noexcept KS_NONBLOCKING {
    sample_rate_ = sample_rate;
    fade_frames_ = to_frames(config_.fade, sample_rate);
    release_step_ = release_step(config_.release, sample_rate);
    fade_position_ = std::min(fade_position_, fade_frames_);
}

void SafetyStage::process(const Block& block, bool muted) noexcept KS_NONBLOCKING {
    if (block.sample_rate != sample_rate_) {
        set_sample_rate(block.sample_rate);
    }
    if (block.resumed) {
        fade_position_ = 0;
    }
    muted_ = muted;
    const std::uint64_t target = muted ? 0 : fade_frames_;

    for (std::size_t i = 0; i < block.frames; ++i) {
        // 1. NaN and Inf become 0, and silence the frame.
        const bool non_finite = zero_non_finite(block, i);
        if (non_finite) {
            fade_position_ = 0;
        }
        // 2. The fade gain.
        const float peak = scale(block, i, fade_gain(muted, non_finite));
        // 3 and 4. The limiter gain, then the clamp.
        const float limiter_gain = limit(peak);
        for (const std::span<float> channel : block.channels) {
            float& sample = channel[i];
            sample = std::clamp(sample * limiter_gain, -ceiling_, ceiling_);
        }
        // 5. The fade takes one step toward its target.
        if (fade_position_ < target) {
            ++fade_position_;
        } else if (fade_position_ > target) {
            --fade_position_;
        }
    }
}

bool SafetyStage::zero_non_finite(const Block& block, std::size_t frame) noexcept KS_NONBLOCKING {
    bool found = false;
    for (const std::span<float> channel : block.channels) {
        float& sample = channel[frame];
        if (!std::isfinite(sample)) {
            sample = 0.0F;
            ++non_finite_samples_;
            found = true;
        }
    }
    return found;
}

float SafetyStage::fade_gain(bool muted, bool non_finite) const noexcept KS_NONBLOCKING {
    if (fade_frames_ != 0) {
        return static_cast<float>(fade_position_) / static_cast<float>(fade_frames_);
    }
    return muted || non_finite ? 0.0F : 1.0F;
}

float SafetyStage::scale(const Block& block, std::size_t frame,
                         float gain) noexcept KS_NONBLOCKING {
    float peak = 0.0F;
    for (const std::span<float> channel : block.channels) {
        float& sample = channel[frame];
        sample *= gain;
        peak = std::max(peak, std::abs(sample));
    }
    return peak;
}

float SafetyStage::limit(float peak) noexcept KS_NONBLOCKING {
    const double target =
        peak > ceiling_ ? static_cast<double>(ceiling_) / static_cast<double>(peak) : 1.0;
    if (target < limiter_gain_) {
        limiter_gain_ = target; // instant attack
    } else {
        limiter_gain_ += (target - limiter_gain_) * release_step_;
        // Only when releasing to 1: snapping to 1 while the target is below
        // it would flip between the two on every frame.
        if (target == 1.0 && 1.0 - limiter_gain_ <= kReleaseSnap) {
            limiter_gain_ = 1.0;
        }
    }
    if (limiter_gain_ < 1.0) {
        ++limited_frames_;
    }
    return static_cast<float>(limiter_gain_);
}

bool SafetyStage::silenced() const noexcept KS_NONBLOCKING {
    return muted_ && fade_position_ == 0;
}

std::uint64_t SafetyStage::non_finite_samples() const noexcept KS_NONBLOCKING {
    return non_finite_samples_;
}

std::uint64_t SafetyStage::limited_frames() const noexcept KS_NONBLOCKING {
    return limited_frames_;
}

const SafetyConfig& SafetyStage::config() const noexcept KS_NONBLOCKING {
    return config_;
}

} // namespace ks::engine
