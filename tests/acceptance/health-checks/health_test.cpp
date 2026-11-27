// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/health-checks/spec.md, step 10a, in the test's
// own process: the health block's layout (requirement 12), the engine's peak
// load (requirement 9), the load check over 10 s (requirements 10 and 13),
// the power profile and source read from a fake /sys (requirements 7 and 8),
// and ks-engine's --sysfs-root (requirement 17).
//
// The peak-load tests drive the engine on the test's threads, as the render
// core would, so under the rtsan preset RealtimeSanitizer checks
// rendered() and take_peak_load().

#include <ks/engine_app/health.h>

#include <ks/engine/block.h>
#include <ks/engine/engine.h>
#include <ks/engine/render_core.h>
#include <ks/engine_app/options.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using ks::engine::Block;
using ks::engine::Engine;
using ks::engine::EventLine;
using ks::engine::RenderStats;
using ks::engine_app::HealthMonitor;
using ks::engine_app::HealthReadings;
using ks::engine_app::PowerProfileReading;
using ks::ipc::EngineState;
using ks::ipc::EngineStatus;
using ks::ipc::HealthState;
using ks::ipc::LogLevel;
using ks::ipc::ResumeState;
using ks::ipc::Seqlock;

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr std::uint32_t kRate = 48'000;
constexpr std::size_t kFrames = 256;
constexpr std::size_t kChannels = 2;
/// The first pass's time: an arbitrary time, well clear of the clock's epoch.
constexpr Clock::time_point kStart{std::chrono::hours{1}};

// ---- Driving the engine as the render core does ------------------------------------------

/// An engine without a state block, and silent output buffers, rendered as
/// the render core renders it: process(), then rendered() with a load.
class Rig {
public:
    Rig() : engine_{std::make_unique<Engine>()} {
        for (std::size_t c = 0; c < kChannels; ++c) {
            std::span{storage_}[c].assign(kFrames, 0.0F);
            std::span{views_}[c] = std::span{std::span{storage_}[c]};
        }
    }

    /// Renders one block whose render load is `load`.
    void render(double load) {
        const Block block{.channels = views_,
                          .frames = kFrames,
                          .sample_rate = kRate,
                          .host_time = kStart + ks::engine::frames_to_duration(frames_, kRate),
                          .output_latency = {},
                          .resumed = false};
        engine_->process(block);
        ++blocks_;
        frames_ += kFrames;
        peak_ = std::max(peak_, load);
        engine_->rendered(block, RenderStats{.blocks_rendered = blocks_,
                                             .frames_rendered = frames_,
                                             .non_finite_samples = 0,
                                             .limited_frames = 0,
                                             .late_blocks = 0,
                                             .render_time = std::chrono::nanoseconds{1'000},
                                             .render_load = load,
                                             .peak_render_load = peak_});
    }

    [[nodiscard]] Engine& engine() { return *engine_; }

private:
    std::unique_ptr<Engine> engine_;
    std::array<std::vector<float>, kChannels> storage_;
    std::array<std::span<float>, kChannels> views_{};
    std::uint64_t blocks_ = 0;
    std::uint64_t frames_ = 0;
    double peak_ = 0.0;
};

/// The lines in `lines` at `level` that contain `text`.
std::size_t count(const std::vector<EventLine>& lines, LogLevel level, std::string_view text) {
    return static_cast<std::size_t>(std::ranges::count_if(lines, [&](const EventLine& line) {
        return line.level == level && line.text.find(text) != std::string::npos;
    }));
}

double real(std::uint64_t code) noexcept {
    return std::bit_cast<double>(code);
}

/// Readings in which only the load says anything.
HealthReadings load_only(double peak) {
    HealthReadings readings;
    readings.peak_load = peak;
    return readings;
}

// ---- A fake /sys ---------------------------------------------------------------------------

/// A temporary folder laid out as much of /sys as the power checks read.
class FakeSys {
public:
    FakeSys() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "ks-health-sys-XXXXXX").string();
        const char* made = ::mkdtemp(pattern.data());
        REQUIRE(made != nullptr);
        path_ = made;
    }
    FakeSys(const FakeSys&) = delete;
    FakeSys& operator=(const FakeSys&) = delete;
    FakeSys(FakeSys&&) = delete;
    FakeSys& operator=(FakeSys&&) = delete;
    ~FakeSys() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    void write(const std::filesystem::path& relative, std::string_view value) const {
        const std::filesystem::path file = path_ / relative;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out{file, std::ios::binary | std::ios::trunc};
        out << value << '\n';
        REQUIRE(out.good());
    }

    void cpu(int cpu, std::string_view driver, std::string_view governor,
             std::string_view epp = {}) const {
        const std::filesystem::path dir =
            std::filesystem::path{"devices/system/cpu"} / ("cpu" + std::to_string(cpu)) / "cpufreq";
        write(dir / "scaling_driver", driver);
        write(dir / "scaling_governor", governor);
        if (!epp.empty()) {
            write(dir / "energy_performance_preference", epp);
        }
    }

    void supply(std::string_view name, std::string_view type, std::string_view field,
                std::string_view value) const {
        const std::filesystem::path dir = std::filesystem::path{"class/power_supply"} / name;
        write(dir / "type", type);
        write(dir / field, value);
    }

private:
    std::filesystem::path path_;
};

// ---- ks-engine's options ---------------------------------------------------------------------

std::expected<ks::engine_app::Options, std::string> parse(const std::vector<std::string>& extra) {
    std::vector<std::string> args{"--supervisor-fd", "3",   "--state-fd", "4",
                                  "--runtime-dir",   "/tmp"};
    args.insert(args.end(), extra.begin(), extra.end());
    const std::vector<std::string_view> views{args.begin(), args.end()};
    return ks::engine_app::parse_options(views);
}

} // namespace

TEST_CASE("Health: the block's layout", "[health]") {
    STATIC_REQUIRE(sizeof(HealthState) == 15 * sizeof(std::uint64_t));
    STATIC_REQUIRE(offsetof(HealthState, passes) == 0);
    STATIC_REQUIRE(offsetof(HealthState, host_time) == 8);
    STATIC_REQUIRE(offsetof(HealthState, known) == 16);
    STATIC_REQUIRE(offsetof(HealthState, failing) == 24);
    STATIC_REQUIRE(offsetof(HealthState, policy) == 32);
    STATIC_REQUIRE(offsetof(HealthState, priority) == 40);
    STATIC_REQUIRE(offsetof(HealthState, rtprio_limit) == 48);
    STATIC_REQUIRE(offsetof(HealthState, memlock_limit) == 56);
    STATIC_REQUIRE(offsetof(HealthState, process_lock) == 64);
    STATIC_REQUIRE(offsetof(HealthState, locked_bytes) == 72);
    STATIC_REQUIRE(offsetof(HealthState, unlocked_bytes) == 80);
    STATIC_REQUIRE(offsetof(HealthState, power_profile) == 88);
    STATIC_REQUIRE(offsetof(HealthState, power_source) == 96);
    STATIC_REQUIRE(offsetof(HealthState, peak_load) == 104);
    STATIC_REQUIRE(offsetof(HealthState, awake) == 112);

    STATIC_REQUIRE(ks::ipc::kHealthRealtime == 1);
    STATIC_REQUIRE(ks::ipc::kHealthMemory == 2);
    STATIC_REQUIRE(ks::ipc::kHealthPowerProfile == 4);
    STATIC_REQUIRE(ks::ipc::kHealthPowerSource == 8);
    STATIC_REQUIRE(ks::ipc::kHealthLoad == 16);
    STATIC_REQUIRE(ks::ipc::kHealthAwake == 32);

    STATIC_REQUIRE(ks::ipc::kEngineStateLayoutVersion == 3);
    // Everything before the health block is where it was in version 2.
    STATIC_REQUIRE(offsetof(EngineState, decks) == 0);
    STATIC_REQUIRE(offsetof(EngineState, status) ==
                   offsetof(EngineState, mixer) + sizeof(Seqlock<ks::ipc::MixerState>));
    STATIC_REQUIRE(offsetof(EngineState, resume) ==
                   offsetof(EngineState, status) + sizeof(Seqlock<EngineStatus>));
    // The health block comes after the resume slots, and ends the layout.
    STATIC_REQUIRE(offsetof(EngineState, health) ==
                   offsetof(EngineState, resume) +
                       (ks::ipc::kResumeSlots * sizeof(Seqlock<ResumeState>)));
    STATIC_REQUIRE(sizeof(EngineState) ==
                   offsetof(EngineState, health) + sizeof(Seqlock<HealthState>));
    STATIC_REQUIRE(ks::ipc::kEngineStateSegmentSize ==
                   ks::ipc::kSegmentHeaderSize + sizeof(EngineState));
}

TEST_CASE("Health: peak load", "[health]") {
    SECTION("taken, then reset") {
        Rig rig;
        rig.render(0.2);
        rig.render(0.9);
        rig.render(0.4);
        CHECK(rig.engine().take_peak_load() == 0.9);
        CHECK(rig.engine().take_peak_load() == 0.0);
        rig.render(0.3);
        CHECK(rig.engine().take_peak_load() == 0.3);
    }

    SECTION("from two threads, no value taken twice and the highest never lost") {
        // Distinct loads in a random order: each one rendered once.
        constexpr std::size_t kBlocks = 100'000;
        std::vector<double> loads(kBlocks);
        for (std::size_t i = 0; i < kBlocks; ++i) {
            loads[i] = static_cast<double>(i + 1) / 50'000.0; // up to 2.0
        }
        std::mt19937_64 random{0x5EED};
        std::ranges::shuffle(loads, random);

        Rig rig;
        std::atomic<bool> done{false};
        std::vector<double> taken;
        std::thread control{[&] {
            while (!done.load(std::memory_order_acquire)) {
                if (const double peak = rig.engine().take_peak_load(); peak > 0.0) {
                    taken.push_back(peak);
                }
            }
            if (const double peak = rig.engine().take_peak_load(); peak > 0.0) {
                taken.push_back(peak);
            }
        }};
        for (const double load : loads) {
            rig.render(load);
        }
        done.store(true, std::memory_order_release);
        control.join();

        REQUIRE_FALSE(taken.empty());
        // The highest load is taken.
        CHECK(std::ranges::max(taken) == std::ranges::max(loads));
        // Every value taken is a load that was rendered, and none twice.
        const std::set<double> rendered{loads.begin(), loads.end()};
        CHECK(std::ranges::all_of(taken, [&](double peak) { return rendered.contains(peak); }));
        const std::set<double> distinct{taken.begin(), taken.end()};
        CHECK(distinct.size() == taken.size());
        // Nothing is left over.
        CHECK(rig.engine().take_peak_load() == 0.0);
    }
}

TEST_CASE("Health: the load check over 10 s", "[health]") {
    SECTION("0.75 fails it for 10 s") {
        HealthMonitor monitor;
        const std::vector<EventLine> first = monitor.pass(kStart, load_only(0.75));
        CAPTURE(first.size());
        CHECK(count(first, LogLevel::Warning,
                    "health: render load reached 0.75 in the last 10 s") == 1);
        CHECK(first.size() == 1);
        CHECK((monitor.state().known & ks::ipc::kHealthLoad) != 0);
        CHECK((monitor.state().failing & ks::ipc::kHealthLoad) != 0);
        CHECK(real(monitor.state().peak_load) == 0.75);
        CHECK(monitor.state().passes == 1);
        CHECK(monitor.state().host_time ==
              static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(kStart.time_since_epoch())
                      .count()));

        // Lower loads since: still failing, and not logged again, up to 9.9 s.
        for (int s = 1; s <= 9; ++s) {
            CHECK(monitor.pass(kStart + seconds{s}, load_only(0.3)).empty());
            CHECK((monitor.state().failing & ks::ipc::kHealthLoad) != 0);
        }
        CHECK(monitor.pass(kStart + milliseconds{9'900}, load_only(0.3)).empty());
        CHECK((monitor.state().failing & ks::ipc::kHealthLoad) != 0);
        CHECK(real(monitor.state().peak_load) == 0.75);

        // At 10 s, the 0.75 is out of the window.
        const std::vector<EventLine> cleared = monitor.pass(kStart + seconds{10}, load_only(0.3));
        CHECK(count(cleared, LogLevel::Info, "health: render load is fine again") == 1);
        CHECK(cleared.size() == 1);
        CHECK((monitor.state().failing & ks::ipc::kHealthLoad) == 0);
        CHECK((monitor.state().known & ks::ipc::kHealthLoad) != 0);
        CHECK(real(monitor.state().peak_load) == 0.3);
        CHECK(monitor.state().passes == 12);
    }

    SECTION("0.69 never fails it") {
        HealthMonitor monitor;
        for (int s = 0; s < 20; ++s) {
            CHECK(monitor.pass(kStart + seconds{s}, load_only(0.69)).empty());
            CHECK((monitor.state().failing & ks::ipc::kHealthLoad) == 0);
        }
        CHECK(real(monitor.state().peak_load) == 0.69);
    }

    SECTION("nothing else known: nothing else logged or failing") {
        HealthMonitor monitor;
        CHECK(monitor.pass(kStart, load_only(0.1)).empty());
        CHECK(monitor.state().known == ks::ipc::kHealthLoad);
        CHECK(monitor.state().failing == 0);
    }
}

TEST_CASE("Health: power from a fake sysfs", "[health]") {
#ifndef __linux__
    SKIP("the power checks read /sys on Linux only");
#else
    using ks::engine_app::read_power_profile;
    using ks::engine_app::read_power_source;

    SECTION("profiles that are power-saving, and say which file") {
        SECTION("platform_profile low-power") {
            const FakeSys sys;
            sys.write("firmware/acpi/platform_profile", "low-power");
            sys.cpu(0, "intel_pstate", "powersave", "balance_performance");
            const PowerProfileReading reading = read_power_profile(sys.path());
            CHECK(reading.profile == ks::ipc::kPowerProfileSaving);
            CHECK(reading.file == "firmware/acpi/platform_profile");
            CHECK(reading.value == "low-power");
        }
        SECTION("one CPU's energy_performance_preference power") {
            const FakeSys sys;
            sys.cpu(0, "intel_pstate", "powersave", "balance_performance");
            sys.cpu(1, "intel_pstate", "powersave", "power");
            const PowerProfileReading reading = read_power_profile(sys.path());
            CHECK(reading.profile == ks::ipc::kPowerProfileSaving);
            CHECK(reading.file == "devices/system/cpu/cpu1/cpufreq/energy_performance_preference");
            CHECK(reading.value == "power");
        }
        SECTION("powersave with acpi-cpufreq") {
            const FakeSys sys;
            sys.cpu(0, "acpi-cpufreq", "powersave");
            const PowerProfileReading reading = read_power_profile(sys.path());
            CHECK(reading.profile == ks::ipc::kPowerProfileSaving);
            CHECK(reading.file == "devices/system/cpu/cpu0/cpufreq/scaling_governor");
            CHECK(reading.value == "powersave");
        }
    }

    SECTION("profiles that aren't") {
        const std::string driver =
            GENERATE(std::string{"intel_pstate"}, std::string{"amd-pstate-epp"});
        CAPTURE(driver);
        const FakeSys sys;
        sys.cpu(0, driver, "powersave", "balance_performance");
        sys.cpu(1, driver, "powersave", "balance_performance");
        CHECK(read_power_profile(sys.path()).profile == ks::ipc::kPowerProfileNormal);

        sys.write("firmware/acpi/platform_profile", "balanced");
        CHECK(read_power_profile(sys.path()).profile == ks::ipc::kPowerProfileNormal);
    }

    SECTION("an empty root: both unknown, and nothing logged") {
        const FakeSys sys;
        const PowerProfileReading profile = read_power_profile(sys.path());
        CHECK(profile.profile == ks::ipc::kPowerProfileUnknown);
        CHECK(read_power_source(sys.path()) == ks::ipc::kPowerSourceUnknown);

        HealthMonitor monitor;
        HealthReadings readings;
        readings.power_profile = profile;
        readings.power_source = ks::ipc::kPowerSourceUnknown;
        CHECK(monitor.pass(kStart, readings).empty());
        CHECK((monitor.state().known &
               (ks::ipc::kHealthPowerProfile | ks::ipc::kHealthPowerSource)) == 0);
    }

    SECTION("power sources") {
        SECTION("a battery discharging, mains offline: battery") {
            const FakeSys sys;
            sys.supply("BAT0", "Battery", "status", "Discharging");
            sys.supply("AC", "Mains", "online", "0");
            CHECK(read_power_source(sys.path()) == ks::ipc::kPowerSourceBattery);

            HealthMonitor monitor;
            HealthReadings readings;
            readings.power_source = ks::ipc::kPowerSourceBattery;
            const std::vector<EventLine> lines = monitor.pass(kStart, readings);
            CHECK(count(lines, LogLevel::Warning, "health: running on battery") == 1);
            CHECK((monitor.state().failing & ks::ipc::kHealthPowerSource) != 0);
        }
        SECTION("mains online: mains") {
            const FakeSys sys;
            sys.supply("BAT0", "Battery", "status", "Discharging");
            sys.supply("AC", "Mains", "online", "1");
            CHECK(read_power_source(sys.path()) == ks::ipc::kPowerSourceMains);
        }
        SECTION("a USB supply online: mains") {
            const FakeSys sys;
            sys.supply("BAT0", "Battery", "status", "Discharging");
            sys.supply("ucsi-source-psy-USBC000:001", "USB", "online", "1");
            CHECK(read_power_source(sys.path()) == ks::ipc::kPowerSourceMains);
        }
    }
#endif
}

TEST_CASE("Health: options", "[health]") {
#ifdef __linux__
    SECTION("--sysfs-root takes an absolute folder") {
        const std::expected<ks::engine_app::Options, std::string> parsed =
            parse({"--sysfs-root", "/some/where"});
        REQUIRE(parsed.has_value());
        CHECK(parsed->sysfs_root == std::filesystem::path{"/some/where"});
        // Without it, none.
        const std::expected<ks::engine_app::Options, std::string> without = parse({});
        REQUIRE(without.has_value());
        CHECK_FALSE(without->sysfs_root.has_value());
    }
    SECTION("usage errors") {
        const std::vector<std::string> wrong =
            GENERATE(std::vector<std::string>{"--sysfs-root", "/a", "--sysfs-root", "/b"},
                     std::vector<std::string>{"--sysfs-root", "relative/path"},
                     std::vector<std::string>{"--sysfs-root", "/" + std::string(4'096, 'x')},
                     std::vector<std::string>{"--sysfs-root"});
        CAPTURE(wrong);
        const std::expected<ks::engine_app::Options, std::string> parsed = parse(wrong);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().starts_with("ks-engine: "));
        CHECK(parsed.error().find("--sysfs-root") != std::string::npos);
    }
    SECTION("a path of 4,096 bytes is accepted") {
        const std::string path = "/" + std::string(4'095, 'x');
        const std::expected<ks::engine_app::Options, std::string> parsed =
            parse({"--sysfs-root", path});
        CHECK(parsed.has_value());
    }
#else
    SECTION("--sysfs-root is a usage error on macOS") {
        const std::expected<ks::engine_app::Options, std::string> parsed =
            parse({"--sysfs-root", "/some/where"});
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().find("--sysfs-root") != std::string::npos);
    }
#endif
    SECTION("--help doesn't list it") {
        CHECK(ks::engine_app::usage().find("--sysfs-root") == std::string::npos);
    }
}
