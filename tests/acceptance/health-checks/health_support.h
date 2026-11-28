// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Helpers for the health-check process tests (specs/health-checks/spec.md,
// step 10a): kool_spin on miniaudio's null backend, started through
// ks_rlimit_exec with the limits a test asks for; a fake /sys; what
// `ks-play health` prints; the health block read straight from the
// engine-state segment; the engine's log; and /proc. Built on the
// engine-restart, ks-play, and processes tests' helpers.

#include <ks/ipc/connection.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/region.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>

#include "play_support.h"
#include "restart_support.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#ifndef KS_RLIMIT_EXEC_PATH
#error "KS_RLIMIT_EXEC_PATH must name the ks_rlimit_exec executable"
#endif

namespace ks::health_checks::test {

namespace restart = ks::engine_restart::test;
namespace play = ks::ks_play::test;
using processes::test::Clock;
using processes::test::eventually;
using processes::test::Milliseconds;
using processes::test::Process;

inline constexpr std::string_view kRlimitExecPath = KS_RLIMIT_EXEC_PATH;

/// Waits are this many times longer under AddressSanitizer or
/// ThreadSanitizer, whose processes run several times slower.
inline constexpr int kSlack = restart::kSlowSanitizer ? 3 : 1;

/// requirement 19's fields, in order.
inline const std::vector<std::string>& health_fields() {
    static const std::vector<std::string> kFields{
        "failing",       "realtime",     "policy",        "priority",     "rtprio_limit",
        "memory",        "process_lock", "memlock_limit", "locked_bytes", "unlocked_bytes",
        "power_profile", "power_source", "peak_load",     "awake"};
    return kFields;
}

// ---- Limits ----------------------------------------------------------------------------

/// A soft and a hard limit, each a number, `unlimited`, or `keep`.
struct Limit {
    std::string soft;
    std::string hard;
};

/// The limits a session's processes start with.
struct Limits {
    std::optional<Limit> rtprio;
    std::optional<Limit> memlock;
};

/// This process's hard limit of `resource`.
inline rlim_t hard_limit(int resource) {
    rlimit current{};
    REQUIRE(::getrlimit(resource, &current) == 0);
    return current.rlim_max;
}

/// Root ignores RLIMIT_RTPRIO and RLIMIT_MEMLOCK (CAP_SYS_NICE, CAP_IPC_LOCK).
inline bool is_root() noexcept {
    return ::geteuid() == 0;
}

// ---- A fake /sys -----------------------------------------------------------------------

/// A folder laid out as much of /sys as the power checks read.
class FakeSys {
public:
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return dir_.path(); }

    /// Writes `value` and a newline, as sysfs does, to `relative`, making
    /// its folders.
    void write(const std::filesystem::path& relative, std::string_view value) const {
        const std::filesystem::path file = dir_.path() / relative;
        std::filesystem::create_directories(file.parent_path());
        // Written beside it and renamed over it, so the engine never reads
        // half a value.
        const std::filesystem::path temp = file.string() + ".tmp";
        {
            std::ofstream out{temp, std::ios::binary | std::ios::trunc};
            out << value << '\n';
            REQUIRE(out.good());
        }
        std::filesystem::rename(temp, file);
    }

    /// Writes CPU `cpu`'s cpufreq files.
    void cpu(int cpu, std::string_view driver, std::string_view governor,
             std::optional<std::string_view> epp = std::nullopt) const {
        const std::filesystem::path dir =
            std::filesystem::path{"devices/system/cpu"} / ("cpu" + std::to_string(cpu)) / "cpufreq";
        write(dir / "scaling_driver", driver);
        write(dir / "scaling_governor", governor);
        if (epp) {
            write(dir / "energy_performance_preference", *epp);
        }
    }

    void platform_profile(std::string_view value) const {
        write("firmware/acpi/platform_profile", value);
    }

    /// A power supply `name` of `type`, with `online` (Mains, USB) or
    /// `status` (Battery).
    void supply(std::string_view name, std::string_view type, std::string_view field,
                std::string_view value) const {
        const std::filesystem::path dir = std::filesystem::path{"class/power_supply"} / name;
        write(dir / "type", type);
        write(dir / field, value);
    }

private:
    ipc::test::TempDir dir_;
};

// ---- kool_spin -------------------------------------------------------------------------

/// What a session runs with.
struct HealthOptions {
    Limits limits;
    /// After the null backend's options: more for ks-engine.
    std::vector<std::string> engine_options;
    /// In place of the null backend: the paced one, at restart::kRate.
    bool paced = false;
    /// kool_spin's own options, after the defaults.
    std::vector<std::string> supervisor;
};

/// kool_spin in a runtime directory of its own, with ks-engine on the null
/// backend and the real worker, started through ks_rlimit_exec and /bin/sh
/// so that its limits are set and its standard error goes to a file.
/// Whatever happens, kool_spin and any engine left without it are killed
/// when this is destroyed.
class HealthSession {
public:
    explicit HealthSession(const HealthOptions& options = {}) {
        std::vector<std::string> limits;
        const auto add = [&](std::string_view name, const std::optional<Limit>& limit) {
            if (limit) {
                limits.insert(limits.end(), {std::string{name}, limit->soft, limit->hard});
            }
        };
        add("rtprio", options.limits.rtprio);
        add("memlock", options.limits.memlock);
        limits.emplace_back("--");
        limits.emplace_back(processes::test::kKoolSpinPath);
        std::vector<std::string> args{
            "--runtime-dir", dir_.path().string(),
            "--log-dir",     logs().string(),
            "--engine",      std::string{processes::test::kEnginePath},
            "--worker",      std::string{track_loading::test::kWorkerPath}};
        args.insert(args.end(), options.supervisor.begin(), options.supervisor.end());
        args.emplace_back("--");
        if (options.paced) {
            args.insert(args.end(),
                        {"--backend", "paced", "--rate", std::to_string(restart::kRate),
                         "--channels", "2", "--period", std::to_string(restart::kPeriod)});
        } else {
            const std::vector<std::string> null = processes::test::null_backend();
            args.insert(args.end(), null.begin(), null.end());
        }
        args.insert(args.end(), options.engine_options.begin(), options.engine_options.end());
        limits.insert(limits.end(), args.begin(), args.end());
        process_ = processes::test::start("/bin/sh",
                                          logs::test::with_stderr(err(), kRlimitExecPath, limits));
        REQUIRE(process_.pid() > 0);
        const std::filesystem::path socket = dir_.path() / "supervisor.sock";
        static_cast<void>(
            eventually([&] { return std::filesystem::exists(socket) || !process_.running(); },
                       Milliseconds{10'000} * kSlack));
        CAPTURE(stderr_text());
        REQUIRE(std::filesystem::exists(socket));
        REQUIRE(process_.running());
    }

    HealthSession(const HealthSession&) = delete;
    HealthSession& operator=(const HealthSession&) = delete;
    HealthSession(HealthSession&&) = delete;
    HealthSession& operator=(HealthSession&&) = delete;
    ~HealthSession() = default;

    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_.path(); }
    [[nodiscard]] std::filesystem::path logs() const { return dir_.path() / "logs"; }
    [[nodiscard]] std::filesystem::path err() const { return dir_.path() / "stderr"; }
    [[nodiscard]] Process& process() noexcept { return process_; }

    /// The engine's process ID, from its lock; 0 if none is running.
    [[nodiscard]] pid_t engine() const {
        return processes::test::lock_holder(dir_.path(), ipc::kEngineLockName).value_or(0);
    }

    /// The engine's log: everything every engine of this session wrote.
    [[nodiscard]] std::string engine_log() const {
        const std::vector<std::filesystem::path> folders = logs::test::session_folders(logs());
        if (folders.size() != 1) {
            return {};
        }
        return play::slurp(folders.front() / "ks-engine.log");
    }

    /// What kool_spin has written to standard error.
    [[nodiscard]] std::string stderr_text() const { return play::slurp(err()); }

private:
    ipc::test::TempDir dir_;
    processes::test::EngineReaper reaper_{dir_.path()};
    Process process_;
};

/// The lines of `text` that contain `needle`.
inline std::size_t count_lines(const std::string& text, std::string_view needle) {
    std::size_t count = 0;
    for (const std::string& line : play::lines_of(text)) {
        if (line.find(needle) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

// ---- ks-play health --------------------------------------------------------------------

/// One run of `ks-play health`.
struct HealthRun {
    play::Finished finished;
    /// Its one line, parsed, if it printed exactly one.
    std::optional<play::StatusLine> line;

    [[nodiscard]] std::optional<std::string> get(std::string_view name) const {
        return line ? line->get(name) : std::nullopt;
    }
    /// Whether check `name` is in `failing`.
    [[nodiscard]] bool fails(std::string_view name) const {
        const std::optional<std::string> failing = get("failing");
        if (!failing) {
            return false;
        }
        std::istringstream in{*failing};
        for (std::string item; std::getline(in, item, ',');) {
            if (item == name) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] std::uint64_t number(std::string_view name) const {
        const std::optional<std::string> value = get(name);
        if (!value || value->empty() ||
            !std::ranges::all_of(*value, [](char c) { return c >= '0' && c <= '9'; })) {
            return 0;
        }
        return std::stoull(*value);
    }
};

/// Runs `ks-play health` against `dir`.
inline HealthRun run_health(const std::filesystem::path& dir) {
    HealthRun run{.finished = play::run_play(dir, {"health"}), .line = std::nullopt};
    const std::vector<std::string> lines = play::lines_of(run.finished.out);
    if (lines.size() == 1) {
        run.line = play::parse_status_line(lines.front());
    }
    return run;
}

/// Runs `ks-play health` until `done` holds for its result or `timeout`
/// passes, and returns the last run.
template <typename Done>
HealthRun health_until(const std::filesystem::path& dir, Done done,
                       Milliseconds timeout = Milliseconds{5'000} * kSlack) {
    HealthRun last;
    static_cast<void>(eventually(
        [&] {
            last = run_health(dir);
            return last.finished.status == 0 && last.line && done(last);
        },
        timeout));
    return last;
}

// ---- The health block, read straight from the segment ------------------------------------

/// The engine-state segment from a Welcome, mapped read-only, for its
/// health block, which ks-play doesn't print all of (`passes`).
class HealthView {
public:
    /// Says Hello to `dir`'s supervisor, and maps the Welcome's segment.
    static std::expected<HealthView, std::string> open(const std::filesystem::path& dir) {
        std::expected<ipc::Connection, std::error_code> connection = processes::test::connect(dir);
        if (!connection) {
            return std::unexpected{std::string{"can't connect"}};
        }
        if (!processes::test::send(
                *connection,
                ipc::Message{.request_id = 1, .body = ipc::msg::Hello{"health test"}})) {
            return std::unexpected{std::string{"couldn't send Hello"}};
        }
        std::optional<processes::test::Received> welcome = processes::test::receive(*connection);
        if (!welcome || !std::holds_alternative<ipc::msg::Welcome>(welcome->message.body) ||
            welcome->fds.size() != 1) {
            return std::unexpected{std::string{"no Welcome with a segment"}};
        }
        std::expected<ipc::Region, ipc::MapFailure> region =
            ipc::Region::map(welcome->fds.front().get(), ipc::SegmentKind::EngineState,
                             ipc::kEngineStateLayoutVersion, ipc::Access::ReadOnly);
        if (!region) {
            return std::unexpected{std::string{"not an engine-state segment of this layout"}};
        }
        auto* const state = region->at<ipc::EngineState>(ipc::kSegmentHeaderSize);
        if (state == nullptr) {
            return std::unexpected{std::string{"too small for the engine's state"}};
        }
        return HealthView{std::move(*connection), std::move(*region), *state};
    }

    /// The health block, if it could be read.
    [[nodiscard]] std::optional<ipc::HealthState> health() const {
        return ipc::SeqlockReader<ipc::HealthState>{state_->health}.try_read(1'000);
    }

    /// Its pass count; 0 if it couldn't be read.
    [[nodiscard]] std::uint64_t passes() const {
        const std::optional<ipc::HealthState> block = health();
        return block ? block->passes : 0;
    }

private:
    HealthView(ipc::Connection connection, ipc::Region region, ipc::EngineState& state) noexcept
        : connection_{std::move(connection)}, region_{std::move(region)}, state_{&state} {}

    ipc::Connection connection_;
    ipc::Region region_;
    ipc::EngineState* state_;
};

// ---- Tracks ----------------------------------------------------------------------------

/// A 10 s track's bytes in memory: interleaved stereo 32-bit float.
inline constexpr std::uint64_t kTenSecondBytes = std::uint64_t{10} * restart::kRate * 2 * 4;

/// Loads `path` on deck 0 with `ks-play load --wait`, which returns once it
/// has decoded, and plays it.
inline void load_and_play(const std::filesystem::path& dir, const std::filesystem::path& path) {
    const play::Finished loaded = play::run_play(dir, {"load", "0", path.string(), "--wait"});
    CAPTURE(loaded.out, loaded.err);
    REQUIRE(loaded.status == 0);
    const play::Finished played = play::run_play(dir, {"play", "0"});
    CAPTURE(played.out, played.err);
    REQUIRE(played.status == 0);
}

/// Deck 0's position, from `ks-play status`; nothing if it can't tell.
inline std::optional<double> deck0_position(const std::filesystem::path& dir) {
    const play::Finished status = play::run_play(dir, {"status"});
    const std::optional<play::StatusLine> deck =
        play::find_label(play::status_lines(status.out), "deck 0");
    if (!deck) {
        return std::nullopt;
    }
    const std::optional<std::string> position = deck->get("position");
    return position ? std::optional<double>{std::stod(*position)} : std::nullopt;
}

/// True if deck 0 plays on: its position grows over half a second.
inline bool deck0_plays(const std::filesystem::path& dir) {
    const std::optional<double> before = deck0_position(dir);
    std::this_thread::sleep_for(Milliseconds{500});
    const std::optional<double> after = deck0_position(dir);
    return before && after && *after > *before;
}

// ---- /proc -----------------------------------------------------------------------------

/// A `/proc/<pid>/status` field in kB, as bytes; nothing if it isn't there.
inline std::optional<std::uint64_t> proc_status_bytes(pid_t pid, std::string_view field) {
    std::ifstream in{std::filesystem::path{"/proc"} / std::to_string(pid) / "status"};
    for (std::string line; std::getline(in, line);) {
        if (line.starts_with(field) && line.size() > field.size() && line[field.size()] == ':') {
            std::istringstream values{line.substr(field.size() + 1)};
            std::uint64_t kb = 0;
            if (values >> kb) {
                return kb * 1024;
            }
        }
    }
    return std::nullopt;
}

/// The scheduling policy and real-time priority of each of `pid`'s threads,
/// from `/proc/<pid>/task/*/stat` (fields 41 and 40).
inline std::vector<std::pair<int, int>> thread_scheduling(pid_t pid) {
    std::vector<std::pair<int, int>> threads;
    const std::filesystem::path tasks =
        std::filesystem::path{"/proc"} / std::to_string(pid) / "task";
    std::error_code error;
    for (const auto& task : std::filesystem::directory_iterator{tasks, error}) {
        const std::string stat = play::slurp(task.path() / "stat");
        const std::size_t paren = stat.rfind(')');
        if (paren == std::string::npos) {
            continue;
        }
        // After the command's ')', the fields start at field 3.
        std::istringstream in{stat.substr(paren + 1)};
        std::vector<std::string> fields{std::istream_iterator<std::string>{in},
                                        std::istream_iterator<std::string>{}};
        constexpr std::size_t kPriority = 40 - 3;
        constexpr std::size_t kPolicy = 41 - 3;
        if (fields.size() > kPolicy) {
            threads.emplace_back(std::stoi(fields[kPolicy]), std::stoi(fields[kPriority]));
        }
    }
    return threads;
}

} // namespace ks::health_checks::test
