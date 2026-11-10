// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the render-core acceptance tests: buffers, a test signal, graphs
// that record what they're asked to render, a watchdog that turns a hang into
// a failure, and a thread that joins when destroyed.

#include <ks/core/realtime.h>
#include <ks/engine/block.h>
#include <ks/engine/render_core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ks::engine::test {

/// A value for each frame and channel, different from its neighbors, so a
/// sample in the wrong place can't pass a check by accident. In [-0.5, 0.5).
constexpr float signal(std::uint64_t frame, std::size_t channel) noexcept {
    std::uint64_t x = (frame * 0x9E37'79B9'7F4A'7C15ULL) + (channel * 0xBF58'476D'1CE4'E5B9ULL) + 1;
    x ^= x >> 31U;
    x *= 0x94D0'49BB'1331'11EBULL;
    x ^= x >> 29U;
    return (static_cast<float>(x >> 40U) / 16'777'216.0F) - 0.5F;
}

/// Planar buffers, one per channel, each filled with `fill`.
class Planar {
public:
    /// One buffer per entry of `lengths`, holding that many samples.
    Planar(const std::vector<std::size_t>& lengths, float fill) {
        data_.reserve(lengths.size());
        views_.reserve(lengths.size());
        for (const std::size_t length : lengths) {
            views_.emplace_back(data_.emplace_back(length, fill));
        }
    }

    /// `channels` buffers of `frames` samples.
    Planar(std::size_t channels, std::size_t frames, float fill)
        : Planar(std::vector<std::size_t>(channels, frames), fill) {}

    Planar(const Planar&) = delete;
    Planar& operator=(const Planar&) = delete;
    Planar(Planar&&) = delete;
    Planar& operator=(Planar&&) = delete;
    ~Planar() = default;

    [[nodiscard]] std::span<const std::span<float>> channels() const noexcept { return views_; }

    [[nodiscard]] std::span<const float> channel(std::size_t c) const { return views_.at(c); }

    void fill(float value) {
        for (std::vector<float>& channel : data_) {
            std::ranges::fill(channel, value);
        }
    }

    /// True if every sample of every channel is `value`.
    [[nodiscard]] bool all_equal(float value) const {
        return std::ranges::all_of(data_, [value](const std::vector<float>& channel) {
            return std::ranges::all_of(channel, [value](float sample) { return sample == value; });
        });
    }

private:
    std::vector<std::vector<float>> data_;
    std::vector<std::span<float>> views_;
};

/// What a graph was asked to render.
struct BlockRecord {
    std::size_t frames = 0;
    std::size_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::chrono::steady_clock::time_point host_time;
};

/// Writes signal(frame, channel), counting frames from the first block it
/// renders, and records each block, up to a limit. The thread that created it
/// may read the records while another renders.
class SignalGraph final : public Graph {
public:
    explicit SignalGraph(std::size_t max_records) : records_(max_records) {}

    void process(const Block& block) noexcept KS_NONBLOCKING override {
        for (std::size_t c = 0; c < block.channels.size(); ++c) {
            const std::span<float> channel = block.channels[c];
            for (std::size_t i = 0; i < block.frames; ++i) {
                channel[i] = signal(next_frame_ + i, c);
            }
        }
        next_frame_ += block.frames;

        const std::size_t n = recorded_.load(std::memory_order_relaxed);
        if (n < records_.size()) {
            std::span{records_}[n] = BlockRecord{
                .frames = block.frames,
                .channels = block.channels.size(),
                .sample_rate = block.sample_rate,
                .host_time = block.host_time,
            };
            recorded_.store(n + 1, std::memory_order_release);
        }
        calls_.fetch_add(1, std::memory_order_relaxed);
    }

    /// The blocks recorded so far, in order.
    [[nodiscard]] std::span<const BlockRecord> records() const noexcept {
        return std::span{records_}.first(recorded_.load(std::memory_order_acquire));
    }

    /// How many blocks it has rendered, recorded or not.
    [[nodiscard]] std::size_t calls() const noexcept {
        return calls_.load(std::memory_order_relaxed);
    }

private:
    std::vector<BlockRecord> records_;
    std::atomic<std::size_t> recorded_{0};
    std::atomic<std::size_t> calls_{0};
    std::uint64_t next_frame_ = 0;
};

/// Writes nothing, and counts the blocks it's asked to render.
class SilentGraph final : public Graph {
public:
    void process(const Block& /*block*/) noexcept KS_NONBLOCKING override {
        calls_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t calls() const noexcept {
        return calls_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::size_t> calls_{0};
};

/// Fills every channel with one value, and notices if two threads are inside
/// process() at once.
class ConstantGraph final : public Graph {
public:
    explicit ConstantGraph(float value) noexcept : value_{value} {}

    void process(const Block& block) noexcept KS_NONBLOCKING override {
        if (active_.fetch_add(1, std::memory_order_acq_rel) != 0) {
            overlapped_.store(true, std::memory_order_relaxed);
        }
        for (const std::span<float> channel : block.channels) {
            std::ranges::fill(channel.first(block.frames), value_);
        }
        frames_.fetch_add(block.frames, std::memory_order_relaxed);
        active_.fetch_sub(1, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool overlapped() const noexcept {
        return overlapped_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t frames() const noexcept {
        return frames_.load(std::memory_order_relaxed);
    }

private:
    float value_;
    std::atomic<int> active_{0};
    std::atomic<bool> overlapped_{false};
    std::atomic<std::uint64_t> frames_{0};
};

/// Asks its own core to render another block from inside process(), as a
/// second driver would if its callback arrived mid-render.
class ReentrantGraph final : public Graph {
public:
    /// The core and the block to render from inside process().
    void set_inner(RenderCore& core, const Block& inner) noexcept {
        core_ = &core;
        inner_ = inner;
    }

    void process(const Block& /*block*/) noexcept KS_NONBLOCKING override {
        if (core_ != nullptr) {
            inner_result_ = core_->try_render(inner_);
        }
    }

    /// What try_render() returned for the inner block, if it was called.
    [[nodiscard]] std::optional<RenderResult> inner_result() const noexcept {
        return inner_result_;
    }

private:
    RenderCore* core_ = nullptr;
    Block inner_;
    std::optional<RenderResult> inner_result_;
};

/// How long a test waits without progress before failing.
inline constexpr std::chrono::seconds kStallLimit{5};

/// Turns a hang into a failure: call progressed() whenever something moves,
/// and give up once stalled() is true.
class StallWatch {
public:
    void progressed() noexcept { last_progress_ = Clock::now(); }

    [[nodiscard]] bool stalled() const noexcept {
        return Clock::now() - last_progress_ > kStallLimit;
    }

private:
    using Clock = std::chrono::steady_clock;

    Clock::time_point last_progress_ = Clock::now();
};

/// A thread that joins when destroyed. AppleClang 17's libc++ doesn't provide
/// std::jthread.
class JoiningThread {
public:
    template <typename F>
        requires(!std::same_as<std::remove_cvref_t<F>, JoiningThread>)
    explicit JoiningThread(F&& function) : thread_{std::forward<F>(function)} {}

    JoiningThread(const JoiningThread&) = delete;
    JoiningThread& operator=(const JoiningThread&) = delete;
    JoiningThread(JoiningThread&&) noexcept = default;
    JoiningThread& operator=(JoiningThread&&) = delete;

    ~JoiningThread() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    std::thread thread_;
};

} // namespace ks::engine::test
