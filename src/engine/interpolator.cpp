// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/interpolator.h>

namespace ks::engine {

// Stubs: the acceptance tests compile against this API before it's
// implemented (specs/deck/spec.md). The interpolator returns silence.

Interpolator::Interpolator() = default;

StereoSample Interpolator::value(std::span<const float> /*samples*/, std::uint64_t /*ready_frames*/,
                                 TrackPosition /*position*/,
                                 double /*stretch*/) const noexcept KS_NONBLOCKING {
    static_cast<void>(table_.size());
    return {};
}

} // namespace ks::engine
