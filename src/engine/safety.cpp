// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/safety.h>

namespace ks::engine {

// Stubs: the acceptance tests compile against this API before it's
// implemented (specs/safety-stage/spec.md). The stage changes nothing yet.

SafetyStage::SafetyStage(const SafetyConfig& config) noexcept
    : ceiling_{static_cast<float>(config.ceiling_dbfs)}, fade_{config.fade},
      release_{config.release} {}

void SafetyStage::process(const Block& /*block*/, bool /*muted*/) noexcept KS_NONBLOCKING {
    static_cast<void>(ceiling_);
    static_cast<void>(fade_);
    static_cast<void>(release_);
    static_cast<void>(sample_rate_);
    static_cast<void>(fade_frames_);
    static_cast<void>(release_step_);
    static_cast<void>(limiter_gain_);
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

} // namespace ks::engine
