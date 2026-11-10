// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/offline.h>

namespace ks::engine {

// Stub: the acceptance tests compile against this API before it's implemented
// (specs/render-core/spec.md). The stubs make every test fail.

std::size_t render_offline(RenderCore& /*core*/, const OfflineConfig& /*config*/,
                           std::span<const std::span<float>> /*output*/) noexcept KS_NONBLOCKING {
    return 0;
}

} // namespace ks::engine
