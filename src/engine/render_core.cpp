// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/render_core.h>

#include <ks/core/clock.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <span>

#ifdef __x86_64__
#include <xmmintrin.h>
#elifndef __aarch64__
#error "Flushing denormals is implemented for x86-64 and arm64 only"
#endif

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

// Flushing denormals (spec requirement 8): flush_denormals() sets the calling
// thread's floating-point mode to flush them to zero and returns the mode it
// replaced, for restore_mode().
#ifdef __x86_64__

using FpMode = unsigned int;

/// MXCSR's flush-to-zero (FTZ) and denormals-are-zero (DAZ) bits.
constexpr FpMode kFlushDenormals = (1U << 15U) | (1U << 6U);

FpMode flush_denormals() noexcept KS_NONBLOCKING {
    const FpMode saved = _mm_getcsr();
    _mm_setcsr(saved | kFlushDenormals);
    return saved;
}

void restore_mode(FpMode saved) noexcept KS_NONBLOCKING {
    _mm_setcsr(saved);
}

#else // __aarch64__

using FpMode = std::uint64_t;

/// FPCR's flush-to-zero (FZ) bit, which flushes inputs and results.
constexpr FpMode kFlushDenormals = std::uint64_t{1} << 24U;

FpMode flush_denormals() noexcept KS_NONBLOCKING {
    const FpMode saved = __builtin_arm_rsr64("fpcr");
    __builtin_arm_wsr64("fpcr", saved | kFlushDenormals);
    return saved;
}

void restore_mode(FpMode saved) noexcept KS_NONBLOCKING {
    __builtin_arm_wsr64("fpcr", saved);
}

#endif

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

    // The block's render time starts once its driver check has passed.
    const auto start = ks::core::monotonic_now();
    // Read once, so a mute takes effect at a block boundary.
    const bool muted = muted_.load(std::memory_order_acquire);
    silence(block);
    Block rendered = block;
    rendered.resumed = !rendered_any_ || driver != last_driver_;
    const FpMode saved_mode = flush_denormals();
    graph_->process(rendered);
    safety_.process(rendered, muted);
    restore_mode(saved_mode);
    const auto finish = ks::core::monotonic_now();
    rendered_any_ = true;
    last_driver_ = driver;

    const auto render_time = std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start);
    const auto duration = frames_to_duration(block.frames, block.sample_rate);
    // A block too short to last a nanosecond has no meaningful load.
    const double load = duration.count() > 0 ? static_cast<double>(render_time.count()) /
                                                   static_cast<double>(duration.count())
                                             : 0.0;
    const bool late = render_time > duration;

    // Only the token's holder writes these; other threads only read them.
    RenderStats stats;
    stats.blocks_rendered = blocks_rendered_.load(std::memory_order_relaxed) + 1;
    stats.frames_rendered = frames_rendered_.load(std::memory_order_relaxed) + block.frames;
    stats.non_finite_samples = safety_.non_finite_samples();
    stats.limited_frames = safety_.limited_frames();
    stats.late_blocks = late_blocks_.load(std::memory_order_relaxed) + (late ? 1U : 0U);
    stats.render_time = render_time;
    stats.render_load = load;
    stats.peak_render_load = std::max(
        std::bit_cast<double>(peak_render_load_bits_.load(std::memory_order_relaxed)), load);
    stats.output_state = output_state_.load(std::memory_order_acquire);
    stats.muted = muted;
    stats.silenced = muted && safety_.silenced();

    blocks_rendered_.store(stats.blocks_rendered, std::memory_order_relaxed);
    frames_rendered_.store(stats.frames_rendered, std::memory_order_relaxed);
    const auto end = block.host_time + duration;
    rendered_until_ns_.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count(),
        std::memory_order_relaxed);
    non_finite_samples_.store(stats.non_finite_samples, std::memory_order_relaxed);
    limited_frames_.store(stats.limited_frames, std::memory_order_relaxed);
    late_blocks_.store(stats.late_blocks, std::memory_order_relaxed);
    render_time_ns_.store(render_time.count(), std::memory_order_relaxed);
    render_load_bits_.store(std::bit_cast<std::uint64_t>(load), std::memory_order_relaxed);
    peak_render_load_bits_.store(std::bit_cast<std::uint64_t>(stats.peak_render_load),
                                 std::memory_order_relaxed);
    // Last, and release: whoever sees the fade finished also sees the frames
    // that finished it.
    silenced_.store(safety_.silenced(), std::memory_order_release);

    // Still holding the token, so the graph's hook never overlaps a render.
    graph_->rendered(rendered, stats);

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

void RenderCore::set_muted(bool muted) noexcept KS_NONBLOCKING {
    muted_.store(muted, std::memory_order_release);
}

bool RenderCore::silenced() const noexcept KS_NONBLOCKING {
    // silenced_ is from the last rendered block, which may predate an unmute.
    return muted_.load(std::memory_order_acquire) && silenced_.load(std::memory_order_acquire);
}

std::uint64_t RenderCore::non_finite_samples() const noexcept KS_NONBLOCKING {
    return non_finite_samples_.load(std::memory_order_relaxed);
}

std::uint64_t RenderCore::limited_frames() const noexcept KS_NONBLOCKING {
    return limited_frames_.load(std::memory_order_relaxed);
}

const SafetyConfig& RenderCore::safety_config() const noexcept KS_NONBLOCKING {
    return safety_.config();
}

std::uint64_t RenderCore::blocks_rendered() const noexcept KS_NONBLOCKING {
    return blocks_rendered_.load(std::memory_order_relaxed);
}

std::uint64_t RenderCore::late_blocks() const noexcept KS_NONBLOCKING {
    return late_blocks_.load(std::memory_order_relaxed);
}

std::chrono::nanoseconds RenderCore::render_time() const noexcept KS_NONBLOCKING {
    return std::chrono::nanoseconds{render_time_ns_.load(std::memory_order_relaxed)};
}

double RenderCore::render_load() const noexcept KS_NONBLOCKING {
    return std::bit_cast<double>(render_load_bits_.load(std::memory_order_relaxed));
}

double RenderCore::peak_render_load() const noexcept KS_NONBLOCKING {
    return std::bit_cast<double>(peak_render_load_bits_.load(std::memory_order_relaxed));
}

bool RenderCore::muted() const noexcept KS_NONBLOCKING {
    return muted_.load(std::memory_order_acquire);
}

void RenderCore::set_output_state(OutputState state) noexcept KS_NONBLOCKING {
    output_state_.store(state, std::memory_order_release);
}

OutputState RenderCore::output_state() const noexcept KS_NONBLOCKING {
    return output_state_.load(std::memory_order_acquire);
}

} // namespace ks::engine
