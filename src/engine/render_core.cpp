// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/render_core.h>

#include <algorithm>
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

    // Read once, so a mute takes effect at a block boundary.
    const bool muted = muted_.load(std::memory_order_acquire);
    silence(block);
    Block rendered = block;
    rendered.resumed = !rendered_any_ || driver != last_driver_;
    const FpMode saved_mode = flush_denormals();
    graph_->process(rendered);
    safety_.process(rendered, muted);
    restore_mode(saved_mode);
    rendered_any_ = true;
    last_driver_ = driver;

    // Only the token's holder writes these; other threads only read them.
    frames_rendered_.store(frames_rendered_.load(std::memory_order_relaxed) + block.frames,
                           std::memory_order_relaxed);
    const auto end = block.host_time + frames_to_duration(block.frames, block.sample_rate);
    rendered_until_ns_.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count(),
        std::memory_order_relaxed);
    non_finite_samples_.store(safety_.non_finite_samples(), std::memory_order_relaxed);
    limited_frames_.store(safety_.limited_frames(), std::memory_order_relaxed);
    // Last, and release: whoever sees the fade finished also sees the frames
    // that finished it.
    silenced_.store(safety_.silenced(), std::memory_order_release);

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

// Stubs (specs/state-out/spec.md requirements 12 to 15): nothing is timed,
// the output state isn't kept, and try_render() doesn't call the graph's
// rendered() yet.
// NOLINTBEGIN(readability-convert-member-functions-to-static): stubs

std::uint64_t RenderCore::blocks_rendered() const noexcept KS_NONBLOCKING {
    return 0;
}

std::uint64_t RenderCore::late_blocks() const noexcept KS_NONBLOCKING {
    return 0;
}

std::chrono::nanoseconds RenderCore::render_time() const noexcept KS_NONBLOCKING {
    return {};
}

double RenderCore::render_load() const noexcept KS_NONBLOCKING {
    return 0.0;
}

double RenderCore::peak_render_load() const noexcept KS_NONBLOCKING {
    return 0.0;
}

bool RenderCore::muted() const noexcept KS_NONBLOCKING {
    return false;
}

void RenderCore::set_output_state(OutputState /*state*/) noexcept KS_NONBLOCKING {}

OutputState RenderCore::output_state() const noexcept KS_NONBLOCKING {
    return OutputState::Stopped;
}

// NOLINTEND(readability-convert-member-functions-to-static)

} // namespace ks::engine
