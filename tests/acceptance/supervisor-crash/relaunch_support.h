// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Helpers for the supervisor-crash acceptance tests (specs/supervisor-crash/
// spec.md) that run kool_spin: killing it, and launching it again, with or
// without a terminal.

#include <ks/ipc/fd.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/runtime_dir.h>
#include <ks/ipc/state.h>

#include "crash_support.h"
#include "restart_support.h"
#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef KS_ON_TERMINAL_PATH
#error "KS_ON_TERMINAL_PATH must name the ks_on_terminal executable"
#endif

namespace ks::supervisor_crash::test {

using engine_restart::test::kRate;
using engine_restart::test::kSlowSanitizer;
using engine_restart::test::Playing;
using engine_restart::test::State;
using Files = ipc::test::TempDir;

/// How long a test waits for a launch, which may have to kill an engine.
inline constexpr Milliseconds kLaunchWait{15'000};

/// True if process `pid` has ended: it's gone, or a zombie that whoever
/// adopted it hasn't reaped.
inline bool ended(pid_t pid) {
    if (!processes::test::exists(pid)) {
        return true;
    }
    std::ifstream in{"/proc/" + std::to_string(pid) + "/stat"};
    const std::string stat{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const std::size_t name_end = stat.rfind(')');
    return name_end != std::string::npos && stat.size() > name_end + 2 && stat[name_end + 2] == 'Z';
}

/// True if the engine's heartbeat increases between each check, 100 ms apart,
/// for `duration`.
inline bool keeps_beating(const State& state, Milliseconds duration = Milliseconds{500}) {
    std::uint64_t last = state.heartbeat();
    for (Milliseconds waited{0}; waited < duration; waited += Milliseconds{100}) {
        std::this_thread::sleep_for(Milliseconds{100});
        const std::uint64_t now = state.heartbeat();
        if (now <= last) {
            return false;
        }
        last = now;
    }
    return true;
}

/// Waits up to 5 s for deck `deck` to play, past `frames`, and returns its
/// block, if it does.
inline std::optional<ipc::DeckState> playing_deck(const State& state, std::size_t deck,
                                                  double frames = 0.0) {
    std::optional<ipc::DeckState> last;
    const auto ready = [&] {
        last = state.deck(deck);
        return last && engine_restart::test::loaded(*last) &&
               engine_restart::test::playing(*last) &&
               engine_restart::test::real_of(last->position) > frames;
    };
    static_cast<void>(eventually(ready));
    return ready() ? last : std::nullopt;
}

/// kool_spin playing `file` on deck 0, with a recording.
inline std::unique_ptr<Playing> playing(const std::filesystem::path& file) {
    auto session = engine_restart::test::start_playing(file);
    session->play(0);
    REQUIRE(playing_deck(session->client.state, 0, 0.5 * kRate).has_value());
    return session;
}

/// kool_spin playing a constant on deck 0, then killed with SIGKILL: its
/// engine is an orphan.
inline std::unique_ptr<Playing> orphaned(const Files& files) {
    auto session = playing(engine_restart::test::write_constant(files.path(), "constant.wav", 120));
    session->session.process().signal(SIGKILL);
    REQUIRE(session->session.process().wait() == 128 + SIGKILL);
    return session;
}

/// kool_spin's arguments for another launch in `dir`, with `options` first,
/// the paced backend, and a recording.
inline std::vector<std::string> launch_args(const std::filesystem::path& dir,
                                            const std::vector<std::string>& options,
                                            std::string_view engine = processes::test::kEnginePath,
                                            std::string_view worker = KS_WORKER_PATH) {
    std::vector<std::string> args{
        "--runtime-dir", dir.string(),        "--log-dir", (dir / "logs").string(),
        "--engine",      std::string{engine}, "--worker",  std::string{worker},
        "--recording"};
    args.insert(args.end(), options.begin(), options.end());
    args.insert(args.end(),
                {"--", "--backend", "paced", "--rate", std::to_string(kRate), "--channels", "2",
                 "--period", std::to_string(engine_restart::test::kPeriod)});
    return args;
}

/// A launch of kool_spin without a terminal: standard input is /dev/null, and
/// standard error a file.
class Launch {
public:
    Launch(const std::filesystem::path& scratch, const std::vector<std::string>& args) {
        static int counter = 0;
        err_ = scratch / ("launch-" + std::to_string(counter++) + ".err");
        std::vector<std::string> shell{"-c", R"(exec "$@" </dev/null 2>"$0")", err_.string(),
                                       std::string{processes::test::kKoolSpinPath}};
        shell.insert(shell.end(), args.begin(), args.end());
        process_ = processes::test::start("/bin/sh", shell);
        REQUIRE(process_.pid() > 0);
    }

    [[nodiscard]] Process& process() noexcept { return process_; }
    [[nodiscard]] std::string err() const {
        std::ifstream in{err_, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }

private:
    std::filesystem::path err_;
    Process process_;
};

/// The path of `master`'s terminal, opened for the launched program.
inline std::string terminal_path(int master) {
    REQUIRE(::grantpt(master) == 0);
    REQUIRE(::unlockpt(master) == 0);
    // NOLINTNEXTLINE(concurrency-mt-unsafe): no other thread asks
    const char* const name = ::ptsname(master);
    REQUIRE(name != nullptr);
    return std::string{name};
}

/// A launch of kool_spin on a terminal of its own, which the test types on and
/// reads.
class TerminalLaunch {
public:
    /// `group` is `foreground` or `background` (on_terminal.cpp).
    explicit TerminalLaunch(const std::vector<std::string>& args,
                            std::string_view group = "foreground")
        : master_{::posix_openpt(O_RDWR | O_NOCTTY)} {
        REQUIRE(master_.valid());
        std::vector<std::string> all{terminal_path(master_.get()), std::string{group},
                                     std::string{processes::test::kKoolSpinPath}};
        all.insert(all.end(), args.begin(), args.end());
        process_ = processes::test::start(KS_ON_TERMINAL_PATH, all);
        REQUIRE(process_.pid() > 0);
    }

    [[nodiscard]] Process& process() noexcept { return process_; }

    /// Reads what the terminal shows, until it shows `text` or `timeout`
    /// passes. True if it showed it.
    [[nodiscard]] bool shows(std::string_view text, Milliseconds timeout = kLaunchWait) {
        const Clock::time_point deadline = Clock::now() + timeout;
        while (shown_.find(text) == std::string::npos) {
            if (Clock::now() >= deadline) {
                return false;
            }
            read(Milliseconds{50});
        }
        return true;
    }

    /// Everything the terminal has shown by the time the launch has exited.
    /// Reads while it waits, so that the launch never blocks on a full
    /// terminal.
    [[nodiscard]] std::optional<int> wait(Milliseconds timeout = kLaunchWait) {
        const Clock::time_point deadline = Clock::now() + timeout;
        while (process_.running() && Clock::now() < deadline) {
            read(Milliseconds{20});
        }
        read(Milliseconds{0});
        return process_.wait(Milliseconds{0});
    }

    [[nodiscard]] const std::string& shown() const noexcept { return shown_; }

    void type(std::string_view text) const {
        REQUIRE(::write(master_.get(), text.data(), text.size()) ==
                static_cast<ssize_t>(text.size()));
    }

private:
    void read(Milliseconds timeout) {
        pollfd polled{.fd = master_.get(), .events = POLLIN, .revents = 0};
        if (::poll(&polled, 1, static_cast<int>(timeout.count())) <= 0) {
            return;
        }
        std::array<char, 4096> bytes{};
        const ssize_t got = ::read(master_.get(), bytes.data(), bytes.size());
        if (got > 0) {
            shown_.append(bytes.data(), static_cast<std::size_t>(got));
        } else {
            // Nothing has the terminal open, yet or any more.
            std::this_thread::sleep_for(Milliseconds{5});
        }
    }

    ipc::Fd master_;
    Process process_;
    std::string shown_;
};

/// True if, within kLaunchWait, an engine other than `old` holds the lock and
/// the supervisor answers a client's Ping.
inline bool new_session(const std::filesystem::path& dir, pid_t old) {
    return eventually(
        [&] {
            const pid_t now = processes::test::lock_holder(dir, ipc::kEngineLockName).value_or(0);
            if (now <= 0 || now == old) {
                return false;
            }
            std::expected<engine_restart::test::Client, std::string> client =
                engine_restart::test::join(dir, "looker");
            return client && processes::test::pings(client->connection, 2);
        },
        kLaunchWait);
}

/// The process ID of `dir`'s engine, from its lock; 0 if none is running.
inline pid_t engine_of(const std::filesystem::path& dir) {
    return processes::test::lock_holder(dir, ipc::kEngineLockName).value_or(0);
}

/// Kills whatever kool_spin runs in `dir`: its engine is an orphan.
inline void kill_supervisor(const std::filesystem::path& dir) {
    const pid_t pid = processes::test::lock_holder(dir, ipc::kSupervisorLockName).value_or(0);
    REQUIRE(pid > 0);
    REQUIRE(::kill(pid, SIGKILL) == 0);
    REQUIRE(eventually(
        [&] { return !processes::test::lock_holder(dir, ipc::kSupervisorLockName).has_value(); }));
}

} // namespace ks::supervisor_crash::test
