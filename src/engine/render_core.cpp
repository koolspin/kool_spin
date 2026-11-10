// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/render_core.h>

#include <algorithm>
#include <span>

namespace ks::engine {

namespace {

bool is_valid(const Block& block) noexcept KS_NONBLOCKING {
    if (block.frames == 0 || block.frames > kMaxBlockFrames) {
        return false;
    }
    if (block.channels.empty() || block.channels.size() > kMaxOutputChannels) {
        return false;
    }
    if (block.sample_rate == 0) {
        return false;
    }
    return std::ranges::all_of(block.channels, [frames = block.frames](std::span<float> channel) {
        return channel.size() >= frames;
    });
}

/// Fills the block's frames of every channel with silence. @pre is_valid(block)
void silence(const Block& block) noexcept KS_NONBLOCKING {
    for (const std::span<float> channel : block.channels) {
        std::ranges::fill(channel.first(block.frames), 0.0F);
    }
}

} // namespace

RenderCore::RenderCore(Graph& graph) noexcept : graph_{&graph} {}

RenderResult RenderCore::try_render(const Block& block) noexcept KS_NONBLOCKING {
    if (!is_valid(block)) {
        return RenderResult::Rejected;
    }
    // Acquire: see everything the previous holder did, on whatever thread.
    if (token_.exchange(true, std::memory_order_acquire)) {
        silence(block);
        return RenderResult::Skipped;
    }
    silence(block);
    graph_->process(block);
    // Only the token's holder writes the count; other threads only read it.
    frames_rendered_.store(frames_rendered_.load(std::memory_order_relaxed) + block.frames,
                           std::memory_order_relaxed);
    // Release: the next holder sees everything this render did.
    token_.store(false, std::memory_order_release);
    return RenderResult::Rendered;
}

std::uint64_t RenderCore::frames_rendered() const noexcept KS_NONBLOCKING {
    return frames_rendered_.load(std::memory_order_relaxed);
}

} // namespace ks::engine
