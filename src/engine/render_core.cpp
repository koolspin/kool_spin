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

RenderCore::RenderCore(Graph& graph, const SafetyConfig& safety) noexcept
    : graph_{&graph}, safety_{safety} {}

RenderResult RenderCore::try_render(const Block& block, DriverId driver) noexcept KS_NONBLOCKING {
    if (!is_valid(block)) {
        return RenderResult::Rejected;
    }
    // Acquire: see everything the previous holder did, on whatever thread.
    if (token_.exchange(true, std::memory_order_acquire)) {
        silence(block);
        return RenderResult::Skipped;
    }
    // Checked while holding the token, so blocks from two drivers never
    // overlap, even while the control thread switches between them.
    if (driver != active_.load(std::memory_order_acquire)) {
        token_.store(false, std::memory_order_release);
        silence(block);
        return RenderResult::Skipped;
    }

    silence(block);
    Block rendered = block;
    rendered.resumed = !rendered_any_ || driver != last_driver_;
    graph_->process(rendered);
    rendered_any_ = true;
    last_driver_ = driver;

    // Only the token's holder writes these; other threads only read them.
    frames_rendered_.store(frames_rendered_.load(std::memory_order_relaxed) + block.frames,
                           std::memory_order_relaxed);
    const auto end = block.host_time + frames_to_duration(block.frames, block.sample_rate);
    rendered_until_ns_.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count(),
        std::memory_order_relaxed);

    // Release: the next holder sees everything this render did.
    token_.store(false, std::memory_order_release);
    return RenderResult::Rendered;
}

std::uint64_t RenderCore::frames_rendered() const noexcept KS_NONBLOCKING {
    return frames_rendered_.load(std::memory_order_relaxed);
}

void RenderCore::activate(DriverId driver) noexcept KS_NONBLOCKING {
    active_.store(driver, std::memory_order_release);
}

DriverId RenderCore::active_driver() const noexcept KS_NONBLOCKING {
    return active_.load(std::memory_order_acquire);
}

std::chrono::steady_clock::time_point RenderCore::rendered_until() const noexcept KS_NONBLOCKING {
    const std::chrono::nanoseconds since_epoch{rendered_until_ns_.load(std::memory_order_relaxed)};
    return std::chrono::steady_clock::time_point{
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(since_epoch)};
}

// Stubs: the safety-stage acceptance tests compile against this API before
// it's implemented (specs/safety-stage/spec.md). The stage changes nothing
// yet, so those tests fail.

void RenderCore::set_muted(bool muted) noexcept KS_NONBLOCKING {
    muted_.store(muted, std::memory_order_relaxed);
}

bool RenderCore::silenced() const noexcept KS_NONBLOCKING {
    return silenced_.load(std::memory_order_relaxed);
}

std::uint64_t RenderCore::non_finite_samples() const noexcept KS_NONBLOCKING {
    return non_finite_samples_.load(std::memory_order_relaxed);
}

std::uint64_t RenderCore::limited_frames() const noexcept KS_NONBLOCKING {
    return limited_frames_.load(std::memory_order_relaxed);
}

} // namespace ks::engine
