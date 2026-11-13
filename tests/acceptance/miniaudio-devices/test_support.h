// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the miniaudio-devices acceptance tests: graphs that count what
// they render, timing checks, and helpers for device keys and errors.

#include <ks/core/realtime.h>
#include <ks/engine/audio_backend.h>
#include <ks/engine/block.h>
#include <ks/engine/miniaudio_backend.h>
#include <ks/engine/render_core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <thread>

namespace ks::engine::test {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

/// Writes silence, and counts the blocks it renders.
class SilentGraph final : public Graph {
public:
    void process(const Block& block) noexcept KS_NONBLOCKING override {
        for (const std::span<float> channel : block.channels) {
            std::ranges::fill(channel.first(block.frames), 0.0F);
        }
        calls_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t calls() const noexcept {
        return calls_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> calls_{0};
};

/// Writes silence, and keeps the smallest and largest output latency of the
/// blocks it renders.
class LatencyGraph final : public Graph {
public:
    void process(const Block& block) noexcept KS_NONBLOCKING override {
        for (const std::span<float> channel : block.channels) {
            std::ranges::fill(channel.first(block.frames), 0.0F);
        }
        const std::int64_t latency = block.output_latency.count();
        if (latency < smallest_.load(std::memory_order_relaxed)) {
            smallest_.store(latency, std::memory_order_relaxed);
        }
        if (latency > largest_.load(std::memory_order_relaxed)) {
            largest_.store(latency, std::memory_order_relaxed);
        }
        calls_.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] std::uint64_t calls() const noexcept {
        return calls_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::chrono::nanoseconds smallest() const noexcept {
        return std::chrono::nanoseconds{smallest_.load(std::memory_order_relaxed)};
    }
    [[nodiscard]] std::chrono::nanoseconds largest() const noexcept {
        return std::chrono::nanoseconds{largest_.load(std::memory_order_relaxed)};
    }

private:
    std::atomic<std::uint64_t> calls_{0};
    std::atomic<std::int64_t> smallest_{std::numeric_limits<std::int64_t>::max()};
    std::atomic<std::int64_t> largest_{std::numeric_limits<std::int64_t>::min()};
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

/// `key` with byte `index` inverted.
inline DeviceKey flipped(DeviceKey key, std::size_t index) {
    std::byte& byte = std::span{key.bytes}[index];
    byte = ~byte;
    return key;
}

/// The error's message, or nothing if there's no error.
template <typename T>
std::string error_text(const std::expected<T, BackendError>& result) {
    return result.has_value() ? std::string{} : result.error().message;
}

} // namespace ks::engine::test
