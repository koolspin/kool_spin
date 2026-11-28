// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine_app/health.h>

#include <ks/engine/audio_events.h>
#include <ks/ipc/log.h>
#include <ks/ipc/state.h>

#include <sys/mman.h>
#include <sys/resource.h>

#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ks::engine_app {

namespace {

/// A burst of render load this high, or higher, fails the load check
/// (requirement 10) ...
constexpr double kLoadLimit = 0.7;
/// ... for this long.
constexpr std::chrono::seconds kLoadWindow{10};

#ifdef __linux__

/// The most files read for the power profile, and for the power source, in
/// one pass: 64 in all (requirement 11).
constexpr std::size_t kProfileFiles = 48;
constexpr std::size_t kSourceFiles = 16;

/// The longest value read from a sysfs file.
constexpr std::size_t kMaxValue = 256;

/// Reads sysfs files, at most a given number of them.
class FileBudget {
public:
    explicit FileBudget(std::size_t files) noexcept : left_{files} {}

    [[nodiscard]] bool spent() const noexcept { return left_ == 0; }

    /// The first line of `file`, without surrounding white space; none if
    /// it can't be read, or the budget is spent.
    std::optional<std::string> read(const std::filesystem::path& file) {
        if (left_ == 0) {
            return std::nullopt;
        }
        --left_;
        std::ifstream in{file, std::ios::binary};
        if (!in) {
            return std::nullopt;
        }
        std::array<char, kMaxValue> buffer{};
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (in.bad()) {
            return std::nullopt;
        }
        std::string_view value{buffer.data(), static_cast<std::size_t>(in.gcount())};
        value = value.substr(0, value.find('\n'));
        constexpr std::string_view kSpace = " \t\r";
        const std::size_t begin = value.find_first_not_of(kSpace);
        if (begin == std::string_view::npos) {
            return std::string{};
        }
        value = value.substr(begin, value.find_last_not_of(kSpace) - begin + 1);
        return std::string{value};
    }

private:
    std::size_t left_;
};

/// The CPUs listed under `root`/devices/system/cpu, as cpu<n>, by number.
std::vector<unsigned> cpus(const std::filesystem::path& root) {
    std::vector<unsigned> numbers;
    std::error_code error;
    for (std::filesystem::directory_iterator it{root / "devices/system/cpu", error}, end;
         !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (!name.starts_with("cpu") || name.size() == 3) {
            continue;
        }
        unsigned number = 0;
        const std::string_view digits = std::string_view{name}.substr(3);
        const char* const end_of_digits = std::to_address(digits.end());
        const auto [last, failed] =
            std::from_chars(std::to_address(digits.begin()), end_of_digits, number);
        if (failed == std::errc{} && last == end_of_digits) {
            numbers.push_back(number);
        }
    }
    std::ranges::sort(numbers);
    return numbers;
}

/// The entries of `root`/class/power_supply, by name.
std::vector<std::string> supplies(const std::filesystem::path& root) {
    std::vector<std::string> names;
    std::error_code error;
    for (std::filesystem::directory_iterator it{root / "class/power_supply", error}, end;
         !error && it != end; it.increment(error)) {
        names.push_back(it->path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

#endif

/// A limit as getrlimit() gives it, with unlimited as ipc::kUnlimited.
std::uint64_t limit_code(rlim_t limit) noexcept {
    return limit == RLIM_INFINITY ? ipc::kUnlimited : static_cast<std::uint64_t>(limit);
}

std::string limit_text(std::uint64_t limit) {
    return limit == ipc::kUnlimited ? std::string{"unlimited"} : std::to_string(limit);
}

std::string_view policy_name(std::uint64_t policy) noexcept {
    switch (policy) {
    case ipc::kPolicyOther:
        return "other";
    case ipc::kPolicyFifo:
        return "fifo";
    case ipc::kPolicyRr:
        return "rr";
    case ipc::kPolicyTimeConstraint:
        return "time-constraint";
    default:
        return "unknown";
    }
}

/// One check's outcome in a pass.
struct Check {
    std::uint64_t bit = 0;
    bool known = false;
    bool failing = false;
    /// The warning when it starts failing, and its name when it's fine again.
    std::string warning;
    std::string_view name;
};

} // namespace

PowerProfileReading read_power_profile([[maybe_unused]] const std::filesystem::path& root) {
#ifdef __linux__
    FileBudget budget{kProfileFiles};
    bool read_any = false;
    const auto saving = [](std::string file, std::string value) {
        return PowerProfileReading{.profile = ipc::kPowerProfileSaving,
                                   .file = std::move(file),
                                   .value = std::move(value)};
    };

    const std::string platform = "firmware/acpi/platform_profile";
    if (const std::optional<std::string> value = budget.read(root / platform)) {
        read_any = true;
        if (*value == "low-power" || *value == "quiet" || *value == "cool") {
            return saving(platform, *value);
        }
    }
    for (const unsigned cpu : cpus(root)) {
        if (budget.spent()) {
            break;
        }
        const std::string dir = std::format("devices/system/cpu/cpu{}/cpufreq/", cpu);
        const std::string epp = dir + "energy_performance_preference";
        if (const std::optional<std::string> value = budget.read(root / epp)) {
            read_any = true;
            if (*value == "power") {
                return saving(epp, *value);
            }
        }
        const std::string governor = dir + "scaling_governor";
        if (const std::optional<std::string> value = budget.read(root / governor)) {
            read_any = true;
            if (*value == "powersave") {
                // intel_pstate's and amd-pstate-epp's powersave is their
                // normal, dynamic governor.
                const std::optional<std::string> driver =
                    budget.read(root / (dir + "scaling_driver"));
                if (driver && *driver != "intel_pstate" && *driver != "amd-pstate-epp") {
                    return saving(governor, *value);
                }
            }
        }
    }
    return PowerProfileReading{.profile =
                                   read_any ? ipc::kPowerProfileNormal : ipc::kPowerProfileUnknown,
                               .file = {},
                               .value = {}};
#else
    // Low Power Mode is read through Foundation, with the Qt UI.
    return {};
#endif
}

std::uint64_t read_power_source([[maybe_unused]] const std::filesystem::path& root) {
#ifdef __linux__
    FileBudget budget{kSourceFiles};
    bool discharging = false;
    bool mains = false;
    for (const std::string& name : supplies(root)) {
        if (budget.spent()) {
            break;
        }
        const std::filesystem::path dir = root / "class/power_supply" / name;
        const std::optional<std::string> type = budget.read(dir / "type");
        if (!type) {
            continue;
        }
        if (*type == "Battery") {
            // A wireless mouse's or keyboard's battery says nothing about the
            // machine's.
            if (budget.read(dir / "scope") == "Device") {
                continue;
            }
            discharging = discharging || budget.read(dir / "status") == "Discharging";
        } else if (*type == "Mains" || *type == "USB") {
            mains = mains || budget.read(dir / "online") == "1";
        }
    }
    if (mains) {
        return ipc::kPowerSourceMains;
    }
    return discharging ? ipc::kPowerSourceBattery : ipc::kPowerSourceUnknown;
#elifdef __APPLE__
    const CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (info == nullptr) {
        return ipc::kPowerSourceUnknown;
    }
    std::uint64_t source = ipc::kPowerSourceUnknown;
    // Owned by `info`.
    if (const CFStringRef type = IOPSGetProvidingPowerSourceType(info); type != nullptr) {
        if (CFEqual(type, CFSTR(kIOPMBatteryPowerKey))) {
            source = ipc::kPowerSourceBattery;
        } else if (CFEqual(type, CFSTR(kIOPMACPowerKey))) {
            source = ipc::kPowerSourceMains;
        }
    }
    CFRelease(info);
    return source;
#else
    return ipc::kPowerSourceUnknown;
#endif
}

bool lock_pages(void* address, std::size_t size) noexcept {
#ifdef __linux__
    // Not mlock(), which the sanitizers intercept (see the declaration).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): syscall() is variadic
    return ::syscall(SYS_mlock, address, size) == 0;
#else
    return ::mlock(address, size) == 0;
#endif
}

ProcessLock lock_process_memory() {
#ifdef __linux__
    // MCL_ONFAULT locks pages only once they're touched, so the track
    // segments' unused space is never committed. Never MCL_FUTURE. Not
    // mlockall(), which the sanitizers intercept (see lock_pages()).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): syscall() is variadic
    if (::syscall(SYS_mlockall, MCL_CURRENT | MCL_ONFAULT) == 0) {
        return ProcessLock{.state = ipc::kProcessLockLocked, .error = {}};
    }
    return ProcessLock{.state = ipc::kProcessLockFailed,
                       .error = std::system_category().message(errno)};
#else
    return {};
#endif
}

std::uint64_t memlock_limit() noexcept {
    rlimit limit{};
    if (::getrlimit(RLIMIT_MEMLOCK, &limit) != 0) {
        return 0;
    }
    return limit_code(limit.rlim_cur);
}

std::uint64_t rtprio_limit() noexcept {
#ifdef RLIMIT_RTPRIO
    rlimit limit{};
    if (::getrlimit(RLIMIT_RTPRIO, &limit) != 0) {
        return 0;
    }
    return limit_code(limit.rlim_cur);
#else
    return 0;
#endif
}

std::vector<engine::EventLine> HealthMonitor::pass(Clock::time_point now,
                                                   const HealthReadings& readings) {
    // The peak loads of the last 10 s, this pass's among them.
    loads_.emplace_back(now, readings.peak_load);
    while (!loads_.empty() && now - loads_.front().first >= kLoadWindow) {
        loads_.pop_front();
    }
    double peak = 0.0;
    for (const auto& load : loads_) {
        peak = std::max(peak, load.second);
    }

    const bool realtime = readings.policy == ipc::kPolicyFifo ||
                          readings.policy == ipc::kPolicyRr ||
                          readings.policy == ipc::kPolicyTimeConstraint;
    const bool process_failed = readings.process_lock == ipc::kProcessLockFailed;
    std::string memory_failure;
    if (process_failed) {
        memory_failure = "mlockall: " + readings.process_lock_error;
    }
    if (readings.unlocked_bytes > 0) {
        if (!memory_failure.empty()) {
            memory_failure += ", ";
        }
        memory_failure += std::format("{} bytes of tracks", readings.unlocked_bytes);
    }

    const std::array<Check, 5> checks{
        Check{.bit = ipc::kHealthRealtime,
              .known = readings.policy != ipc::kPolicyUnknown,
              .failing = !realtime,
              .warning =
                  std::format("health: the audio thread isn't real-time ({}); RLIMIT_RTPRIO "
                              "is {}, and 70 is needed",
                              policy_name(readings.policy), limit_text(readings.rtprio_limit)),
              .name = "real-time priority"},
        Check{.bit = ipc::kHealthMemory,
              .known = readings.process_lock != ipc::kProcessLockNone ||
                       readings.locked_bytes + readings.unlocked_bytes > 0,
              .failing = !memory_failure.empty(),
              .warning = std::format("health: memory isn't locked ({}); RLIMIT_MEMLOCK is {}",
                                     memory_failure, limit_text(readings.memlock_limit)),
              .name = "memory locking"},
        Check{.bit = ipc::kHealthPowerProfile,
              .known = readings.power_profile.profile != ipc::kPowerProfileUnknown,
              .failing = readings.power_profile.profile == ipc::kPowerProfileSaving,
              .warning = std::format("health: the CPU is in a power-saving profile ({} is {})",
                                     readings.power_profile.file, readings.power_profile.value),
              .name = "power profile"},
        Check{.bit = ipc::kHealthPowerSource,
              .known = readings.power_source != ipc::kPowerSourceUnknown,
              .failing = readings.power_source == ipc::kPowerSourceBattery,
              .warning = "health: running on battery",
              .name = "power source"},
        Check{.bit = ipc::kHealthLoad,
              .known = true,
              .failing = peak >= kLoadLimit,
              .warning = std::format("health: render load reached {:.6g} in the last 10 s", peak),
              .name = "render load"},
    };

    std::vector<engine::EventLine> lines;
    std::uint64_t known = 0;
    std::uint64_t failing = 0;
    for (const Check& check : checks) {
        if (!check.known) {
            continue;
        }
        known |= check.bit;
        const bool reported = (reported_ & check.bit) != 0;
        if (check.failing) {
            failing |= check.bit;
            if (!reported) {
                lines.push_back(
                    engine::EventLine{.level = ipc::LogLevel::Warning, .text = check.warning});
                reported_ |= check.bit;
            }
        } else if (reported) {
            lines.push_back(
                engine::EventLine{.level = ipc::LogLevel::Info,
                                  .text = std::format("health: {} is fine again", check.name)});
            reported_ &= ~check.bit;
        }
    }

    state_.passes += 1;
    state_.host_time = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    state_.known = known;
    state_.failing = failing;
    state_.policy = readings.policy;
    state_.priority = readings.priority;
    state_.rtprio_limit = readings.rtprio_limit;
    state_.memlock_limit = readings.memlock_limit;
    state_.process_lock = readings.process_lock;
    state_.locked_bytes = readings.locked_bytes;
    state_.unlocked_bytes = readings.unlocked_bytes;
    state_.power_profile = readings.power_profile.profile;
    state_.power_source = readings.power_source;
    state_.peak_load = std::bit_cast<std::uint64_t>(peak);
    state_.awake = ipc::kAwakeUnknown;
    return lines;
}

} // namespace ks::engine_app
