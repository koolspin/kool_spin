// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The engine's health checks (specs/health-checks/spec.md requirements 7
/// to 13): reading the power profile and source, and the health pass that
/// turns what the control thread has gathered into the health block and its
/// log lines. Everything here runs on the control thread.

#include <ks/engine/audio_events.h>
#include <ks/ipc/state.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace ks::engine_app {

/// The power-profile check's reading (requirement 7).
struct PowerProfileReading {
    /// ipc::kPowerProfileUnknown, kPowerProfileNormal, or kPowerProfileSaving.
    std::uint64_t profile = ipc::kPowerProfileUnknown;
    /// For a saving profile: the file that made it one, relative to the
    /// root (for example `firmware/acpi/platform_profile`), and its value.
    std::string file;
    std::string value;

    bool operator==(const PowerProfileReading&) const = default;
};

/// Requirement 7's power profile, from the files under `root` (`/sys`, or
/// `--sysfs-root`). On macOS, always unknown.
[[nodiscard]] PowerProfileReading read_power_profile(const std::filesystem::path& root);

/// Requirement 8's power source on Linux, from `class/power_supply/*` under
/// `root`: ipc::kPowerSourceUnknown, kPowerSourceMains, or
/// kPowerSourceBattery.
[[nodiscard]] std::uint64_t read_power_source(const std::filesystem::path& root);

/// What the control thread gives a health pass.
struct HealthReadings {
    /// Requirement 6: the active driver thread's scheduling, after asking for
    /// real-time priority (ipc::kPolicy...), its priority, and RLIMIT_RTPRIO's
    /// soft limit.
    std::uint64_t policy = ipc::kPolicyUnknown;
    std::uint64_t priority = 0;
    std::uint64_t rtprio_limit = 0;
    /// Requirements 1 to 3.
    std::uint64_t memlock_limit = 0;
    std::uint64_t process_lock = ipc::kProcessLockNone;
    /// Why the last mlockall() failed, for the log line.
    std::string process_lock_error;
    std::uint64_t locked_bytes = 0;
    std::uint64_t unlocked_bytes = 0;
    /// Requirements 7 and 8.
    PowerProfileReading power_profile;
    std::uint64_t power_source = ipc::kPowerSourceUnknown;
    /// Requirement 9: Engine::take_peak_load() at this pass.
    double peak_load = 0.0;
};

/// The health pass (requirements 10 to 13): it keeps the last 10 s of peak
/// loads, decides each check, and says what to log.
class HealthMonitor {
public:
    using Clock = std::chrono::steady_clock;

    /// Runs one pass at `now` with `readings`: updates state(), and returns
    /// the lines to log for each check whose failing bit changed, in the
    /// order of requirement 13 (real-time, memory, power profile, power
    /// source, render load).
    [[nodiscard]] std::vector<engine::EventLine> pass(Clock::time_point now,
                                                      const HealthReadings& readings);

    /// The health block as the last pass left it; all zero before the first.
    [[nodiscard]] const ipc::HealthState& state() const noexcept { return state_; }

private:
    ipc::HealthState state_{};
    /// The peak load each pass took, and when, for the last 10 s.
    std::deque<std::pair<Clock::time_point, double>> loads_;
};

} // namespace ks::engine_app
