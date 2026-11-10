// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the device-loss acceptance tests: buffers, a test signal, graphs
// that record what they're asked to render, timing checks, and a thread that
// joins when destroyed.

#include <ks/core/realtime.h>
#include <ks/engine/block.h>
#include <ks/engine/render_core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ks::engine::test {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

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
    Planar(std::size_t channels, std::size_t frames, float fill)
        : data_(channels, std::vector<float>(frames, fill)) {
        views_.reserve(channels);
        for (std::vector<float>& channel : data_) {
            views_.emplace_back(channel);
        }
    }

    Planar(const Planar&) = delete;
    Planar& operator=(const Planar&) = delete;
    Planar(Planar&&) = delete;
    Planar& operator=(Planar&&) = delete;
    ~Planar() = default;

    [[nodiscard]] std::span<const std::span<float>> channels() const noexcept { return views_; }

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
    TimePoint host_time;
    bool resumed = false;
    /// How many frames the graph had rendered before this block.
    std::uint64_t first_frame = 0;
};

/// Writes signal(frame, channel), counting frames from the first block it
/// renders, and records each block, up to a limit. Another thread may read
/// the records while it renders.
class RecordingGraph final : public Graph {
public:
    explicit RecordingGraph(std::size_t max_records) : records_(max_records) {}

    void process(const Block& block) noexcept KS_NONBLOCKING override {
        for (std::size_t c = 0; c < block.channels.size(); ++c) {
            const std::span<float> channel = block.channels[c];
            for (std::size_t i = 0; i < block.frames; ++i) {
                channel[i] = signal(next_frame_ + i, c);
            }
        }
        const std::size_t n = recorded_.load(std::memory_order_relaxed);
        if (n < records_.size()) {
            std::span{records_}[n] = BlockRecord{
                .frames = block.frames,
                .channels = block.channels.size(),
                .sample_rate = block.sample_rate,
                .host_time = block.host_time,
                .resumed = block.resumed,
                .first_frame = next_frame_,
            };
            recorded_.store(n + 1, std::memory_order_release);
        }
        next_frame_ += block.frames;
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

/// Fills every channel with 1, counts frames and resumed blocks, and notices
/// if two threads are inside process() at once.
class CountingGraph final : public Graph {
public:
    void process(const Block& block) noexcept KS_NONBLOCKING override {
        if (active_.fetch_add(1, std::memory_order_acq_rel) != 0) {
            overlapped_.store(true, std::memory_order_relaxed);
        }
        for (const std::span<float> channel : block.channels) {
            std::ranges::fill(channel.first(block.frames), 1.0F);
        }
        frames_.fetch_add(block.frames, std::memory_order_relaxed);
        if (block.resumed) {
            resumed_.fetch_add(1, std::memory_order_relaxed);
        }
        calls_.fetch_add(1, std::memory_order_relaxed);
        active_.fetch_sub(1, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool overlapped() const noexcept {
        return overlapped_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t frames() const noexcept {
        return frames_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t resumed() const noexcept {
        return resumed_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t calls() const noexcept {
        return calls_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<int> active_{0};
    std::atomic<bool> overlapped_{false};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> resumed_{0};
    std::atomic<std::uint64_t> calls_{0};
};

/// The tolerance for timing checks.
inline constexpr std::chrono::milliseconds kTimingTolerance{50};

/// How far `frames` at `sample_rate` is from `elapsed`, in seconds.
inline double seconds_off(std::uint64_t frames, std::uint32_t sample_rate,
                          std::chrono::nanoseconds elapsed) {
    const double audio = static_cast<double>(frames) / static_cast<double>(sample_rate);
    return std::abs(audio - std::chrono::duration<double>(elapsed).count());
}

/// True if `frames` at `sample_rate` is within kTimingTolerance of `elapsed`.
inline bool within_tolerance(std::uint64_t frames, std::uint32_t sample_rate,
                             std::chrono::nanoseconds elapsed) {
    return seconds_off(frames, sample_rate, elapsed) <=
           std::chrono::duration<double>(kTimingTolerance).count();
}

/// Sleeps in 1 ms steps until `done()` is true or `timeout` has passed.
/// Returns whether `done()` became true.
template <typename Done>
bool wait_until(Done done, std::chrono::milliseconds timeout) {
    const TimePoint deadline = Clock::now() + timeout;
    while (!done()) {
        if (Clock::now() >= deadline) {
            return done();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

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
