// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/render_core.h>

namespace ks::engine {

// Stub: the acceptance tests compile against this API before it's implemented
// (specs/render-core/spec.md). The stubs make every test fail.

RenderCore::RenderCore(Graph& graph) noexcept : graph_{&graph} {}

RenderResult RenderCore::try_render(const Block& /*block*/) noexcept KS_NONBLOCKING {
    static_cast<void>(graph_);
    static_cast<void>(token_);
    return RenderResult::Rejected;
}

std::uint64_t RenderCore::frames_rendered() const noexcept KS_NONBLOCKING {
    return frames_rendered_.load(std::memory_order_relaxed);
}

} // namespace ks::engine
