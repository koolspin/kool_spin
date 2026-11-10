// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/block.h>

namespace ks::engine {

// Stub: the acceptance tests compile against this API before it's implemented
// (specs/render-core/spec.md). The stubs make every test fail.

std::chrono::nanoseconds frames_to_duration(std::uint64_t /*frames*/,
                                            std::uint32_t /*sample_rate*/) noexcept KS_NONBLOCKING {
    return std::chrono::nanoseconds{-1};
}

} // namespace ks::engine
