// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Reading a thread's scheduling, and asking for real-time priority for it
/// (ADR 0003 §15; specs/health-checks/spec.md requirements 5 and 6). Never
/// called from the audio thread.

#include <pthread.h>

#include <chrono>
#include <cstdint>

namespace ks::engine {

/// The priority a real-time thread asks for on Linux: below PipeWire's data
/// threads (88). It needs RLIMIT_RTPRIO.
inline constexpr int kRealtimePriority = 70;

/// A thread's scheduling policy.
enum class SchedulingPolicy : std::uint8_t {
    /// It couldn't be read.
    Unknown,
    /// Not real-time: SCHED_OTHER, SCHED_BATCH, or SCHED_IDLE on Linux;
    /// anything but a time-constraint policy on macOS.
    Other,
    Fifo,
    RoundRobin,
    /// macOS's time-constraint policy.
    TimeConstraint,
};

struct ThreadScheduling {
    SchedulingPolicy policy = SchedulingPolicy::Unknown;
    /// The thread's priority under SCHED_FIFO or SCHED_RR (Linux); else 0.
    int priority = 0;

    [[nodiscard]] bool realtime() const noexcept {
        return policy == SchedulingPolicy::Fifo || policy == SchedulingPolicy::RoundRobin ||
               policy == SchedulingPolicy::TimeConstraint;
    }

    bool operator==(const ThreadScheduling&) const = default;
};

/// `thread`'s scheduling, which must be a thread that hasn't ended.
[[nodiscard]] ThreadScheduling read_scheduling(pthread_t thread) noexcept;

/// Asks for real-time priority for `thread`: SCHED_FIFO at kRealtimePriority
/// on Linux; on macOS, a time-constraint policy for a thread that renders
/// every `period`. Returns whether it was granted.
bool request_realtime(pthread_t thread, std::chrono::nanoseconds period) noexcept;

/// Reads `thread`'s scheduling; if it isn't real-time, asks for real-time
/// priority and reads it again (requirement 5). A thread that already has
/// real-time priority isn't touched.
[[nodiscard]] ThreadScheduling raise_to_realtime(pthread_t thread,
                                                 std::chrono::nanoseconds period) noexcept;

} // namespace ks::engine
