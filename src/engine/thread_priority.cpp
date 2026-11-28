// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/thread_priority.h>

#include <pthread.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#elifdef __linux__
#include <sched.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace ks::engine {

ThreadScheduling read_scheduling([[maybe_unused]] pthread_t thread) noexcept {
#ifdef __linux__
    int policy = 0;
    sched_param param{};
    if (::pthread_getschedparam(thread, &policy, &param) != 0) {
        return {};
    }
#ifdef SCHED_RESET_ON_FORK
    policy &= ~SCHED_RESET_ON_FORK;
#endif
    switch (policy) {
    case SCHED_FIFO:
        return ThreadScheduling{.policy = SchedulingPolicy::Fifo, .priority = param.sched_priority};
    case SCHED_RR:
        return ThreadScheduling{.policy = SchedulingPolicy::RoundRobin,
                                .priority = param.sched_priority};
    default:
        return ThreadScheduling{.policy = SchedulingPolicy::Other, .priority = 0};
    }
#elifdef __APPLE__
    thread_time_constraint_policy_data_t policy{};
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t get_default = FALSE;
    if (thread_policy_get(pthread_mach_thread_np(thread), THREAD_TIME_CONSTRAINT_POLICY,
                          reinterpret_cast<thread_policy_t>(&policy), &count,
                          &get_default) != KERN_SUCCESS) {
        return {};
    }
    // The default means the thread has no time-constraint policy of its own.
    return ThreadScheduling{.policy = get_default != FALSE ? SchedulingPolicy::Other
                                                           : SchedulingPolicy::TimeConstraint,
                            .priority = 0};
#else
    return {};
#endif
}

bool request_realtime([[maybe_unused]] pthread_t thread,
                      [[maybe_unused]] std::chrono::nanoseconds period) noexcept {
#ifdef __linux__
    sched_param param{};
    param.sched_priority = kRealtimePriority;
    return ::pthread_setschedparam(thread, SCHED_FIFO, &param) == 0;
#elifdef __APPLE__
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0) {
        return false;
    }
    // Mach absolute time units are numer/denom nanoseconds.
    const auto ticks = [&timebase](std::int64_t nanoseconds) {
        const auto value =
            static_cast<std::uint64_t>(nanoseconds) * timebase.denom / timebase.numer;
        return static_cast<std::uint32_t>(
            std::min<std::uint64_t>(value, std::numeric_limits<std::uint32_t>::max()));
    };
    thread_time_constraint_policy_data_t policy{};
    policy.period = ticks(period.count());
    policy.computation = ticks(period.count() / 2);
    policy.constraint = ticks(period.count());
    policy.preemptible = TRUE;
    return thread_policy_set(pthread_mach_thread_np(thread), THREAD_TIME_CONSTRAINT_POLICY,
                             reinterpret_cast<thread_policy_t>(&policy),
                             THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
#else
    return false;
#endif
}

ThreadScheduling raise_to_realtime(pthread_t thread, std::chrono::nanoseconds period) noexcept {
    const ThreadScheduling now = read_scheduling(thread);
    if (now.realtime() || now.policy == SchedulingPolicy::Unknown) {
        return now;
    }
    static_cast<void>(request_realtime(thread, period));
    return read_scheduling(thread);
}

} // namespace ks::engine
