// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/interleave.h>

namespace ks::engine {

// Stub: the acceptance tests compile against this API before it's implemented
// (specs/render-core/spec.md). The stubs make every test fail.

bool interleave(std::span<const std::span<float>> /*planar*/, std::size_t /*frames*/,
                std::span<float> /*interleaved*/) noexcept KS_NONBLOCKING {
    return false;
}

std::size_t
render_interleaved(RenderCore& /*core*/, std::span<const std::span<float>> /*scratch*/,
                   std::span<float> /*interleaved*/, std::size_t /*channels*/,
                   std::uint32_t /*sample_rate*/,
                   std::chrono::steady_clock::time_point /*host_time*/) noexcept KS_NONBLOCKING {
    return 0;
}

} // namespace ks::engine
