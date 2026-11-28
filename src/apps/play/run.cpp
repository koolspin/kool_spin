// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/play_app/run.h>

#include <ks/client/command.h>
#include <ks/client/session.h>
#include <ks/client/snapshot.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/runtime_dir.h>
#include <ks/ipc/state_dir.h>
#include <ks/ipc/track_segment.h>

#include "status_format.h"
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

namespace ks::play_app {
namespace {

using client::Error;
using client::ErrorKind;
using client::Milliseconds;
using client::Session;

/// How long each next_decode_ended() call waits before `load --wait` asks
/// again: `--wait` itself has no limit (requirement 24).
constexpr Milliseconds kDecodePoll{1'000};

/// How long `status --watch` waits for each interval's `Pong`.
constexpr Milliseconds kWatchPing{1'000};

/// How long `health` waits for the engine's first health pass, and how often
/// it looks.
constexpr Milliseconds kHealthWait{2'000};
constexpr Milliseconds kHealthPoll{20};

/// Exit statuses for SIGINT and SIGTERM (requirement 29): 128 + the signal.
constexpr int kExitInterrupted = 130;
constexpr int kExitTerminated = 143;

/// The longest `status --watch` sleeps before it looks at its stop flag.
constexpr Milliseconds kStopCheck{50};

std::string_view error_code_name(ipc::ErrorCode code) noexcept {
    switch (code) {
    case ipc::ErrorCode::BadMessage:
        return "bad message";
    case ipc::ErrorCode::NotSupported:
        return "not supported";
    case ipc::ErrorCode::Unavailable:
        return "unavailable";
    case ipc::ErrorCode::Refused:
        return "refused";
    }
    return "bad message";
}

std::string_view track_status_name(ipc::TrackStatus status) noexcept {
    switch (status) {
    case ipc::TrackStatus::Complete:
        return "complete";
    case ipc::TrackStatus::Truncated:
        return "truncated";
    case ipc::TrackStatus::Decoding:
    case ipc::TrackStatus::Failed:
        return "failed";
    }
    return "failed";
}

/// Reports `error` as one line on `err` (requirement 28), and returns the
/// exit status for a failure after connecting.
int report(std::ostream& err, const Error& error) {
    err << "ks-play: ";
    if (error.kind == ErrorKind::Refused) {
        err << error_code_name(error.code) << ": ";
    }
    err << error.text << '\n';
    return kExitFailed;
}

/// Ends a command whose result went to `out`: a write that failed (a closed
/// standard output, requirement 29) is a failure.
int finish(std::ostream& out, std::ostream& err) {
    out.flush();
    if (!out) {
        err << "ks-play: can't write to standard output\n";
        return kExitFailed;
    }
    return kExitOk;
}

int do_load(Session& session, const Load& load, std::ostream& out, std::ostream& err) {
    const client::Result<client::LoadedFile> loaded = session.load_file(load.deck, load.path);
    if (!loaded) {
        return report(err, loaded.error());
    }
    out << "loaded track " << loaded->track_id << " on deck " << load.deck << ": "
        << loaded->sample_rate << " Hz, estimated " << loaded->estimated_frames << " frames\n";
    if (const int written = finish(out, err); written != kExitOk || !load.wait) {
        return written;
    }
    for (;;) {
        const client::Result<ipc::msg::DecodeEnded> ended = session.next_decode_ended(kDecodePoll);
        if (!ended) {
            if (ended.error().kind == ErrorKind::Timeout) {
                continue;
            }
            return report(err, ended.error());
        }
        if (ended->track_id != loaded->track_id) {
            continue;
        }
        out << "decoded " << ended->frames << " frames: " << track_status_name(ended->status)
            << '\n';
        const int written = finish(out, err);
        if (written != kExitOk) {
            return written;
        }
        return ended->status == ipc::TrackStatus::Complete ? kExitOk : kExitFailed;
    }
}

int do_unload(Session& session, const Unload& unload, std::ostream& out, std::ostream& err) {
    if (const client::Result<void> done = session.unload_deck(unload.deck); !done) {
        return report(err, done.error());
    }
    out << "unloaded deck " << unload.deck << '\n';
    return finish(out, err);
}

int do_control(Session& session, const Control& control, std::ostream& out, std::ostream& err) {
    if (const client::Result<std::uint32_t> ring = session.open_ring(); !ring) {
        return report(err, ring.error());
    }
    const client::Result<std::uint64_t> sequence = session.send(control.command);
    if (!sequence) {
        return report(err, sequence.error());
    }
    const client::Result<client::Applied> applied = session.wait_applied(*sequence);
    if (!applied) {
        return report(err, applied.error());
    }
    switch (applied->outcome) {
    case client::Outcome::Applied:
        out << "ok\n";
        return finish(out, err);
    case client::Outcome::Rejected:
        err << "ks-play: the engine rejected the command: "
            << client::rejection_name(applied->rejection) << '\n';
        return kExitFailed;
    case client::Outcome::Corrupt:
        err << "ks-play: the ring's slot is corrupt\n";
        return kExitFailed;
    case client::Outcome::TimedOut:
        break;
    }
    err << "ks-play: timed out waiting for the engine to apply the command\n";
    return kExitFailed;
}

/// Prints one snapshot and flushes it.
int print_snapshot(Session& session, std::ostream& out, std::ostream& err) {
    const client::Result<client::Snapshot> snapshot = session.snapshot();
    if (!snapshot) {
        return report(err, snapshot.error());
    }
    format_status(*snapshot, out);
    return finish(out, err);
}

/// Sleeps for `interval` in steps of at most kStopCheck. Returns false, early,
/// once `stop` is set.
bool sleep_unless_stopped(Milliseconds interval, const std::atomic<bool>& stop) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + interval;
    for (;;) {
        if (stop.load(std::memory_order_acquire)) {
            return false;
        }
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return true;
        }
        std::this_thread::sleep_for(
            std::min<std::chrono::steady_clock::duration>(deadline - now, kStopCheck));
    }
}

/// `status`, and `status --watch` (requirement 25). A watch asks the
/// supervisor for a `Pong` every interval: the state segment stays mapped
/// after kool_spin dies, so only the connection tells (plan, "Decisions").
/// It ends with status 0 once `stop` is set (requirement 29).
int do_status(Session& session, const Status& status, const std::atomic<bool>& stop,
              std::ostream& out, std::ostream& err) {
    if (!status.watch) {
        return print_snapshot(session, out, err);
    }
    const Milliseconds interval{status.interval_ms};
    for (bool first = true;; first = false) {
        if (stop.load(std::memory_order_acquire)) {
            return kExitOk;
        }
        if (!first) {
            out << "---\n";
        }
        if (const int printed = print_snapshot(session, out, err); printed != kExitOk) {
            return printed;
        }
        if (!sleep_unless_stopped(interval, stop)) {
            return kExitOk;
        }
        if (const client::Result<void> alive = session.ping(kWatchPing); !alive) {
            return report(err, alive.error());
        }
    }
}

/// `health` (specs/health-checks/spec.md requirement 19): waits up to
/// kHealthWait for the engine's first health pass, then prints it.
int do_health(Session& session, std::ostream& out, std::ostream& err) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + kHealthWait;
    for (;;) {
        const client::Result<client::Snapshot> snapshot = session.snapshot();
        if (!snapshot) {
            return report(err, snapshot.error());
        }
        if (snapshot->health) {
            format_health(*snapshot->health, out);
            return finish(out, err);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            err << "ks-play: no health check yet\n";
            return kExitFailed;
        }
        std::this_thread::sleep_for(kHealthPoll);
    }
}

int do_stop(Session& session, std::ostream& out, std::ostream& err) {
    if (const client::Result<void> done = session.stop(); !done) {
        return report(err, done.error());
    }
    out << "stopped\n";
    return finish(out, err);
}

int do_ping(Session& session, std::ostream& out, std::ostream& err) {
    if (const client::Result<void> done = session.ping(); !done) {
        return report(err, done.error());
    }
    out << "pong\n";
    return finish(out, err);
}

/// The folders whose locks say what runs: each this user's own directory. The
/// state folder is one only if kool_spin would use it.
std::vector<std::filesystem::path> lock_dirs(const Invocation& invocation) {
    std::vector<std::filesystem::path> dirs;
    const ipc::StateDirectoryChoice state =
        ipc::choose_state_directory(invocation.state_dir, !invocation.default_runtime_dir);
    if (state.path &&
        ipc::check_state_directory(*state.path, state.source == ipc::StateDirectorySource::Named)) {
        dirs.push_back(*state.path);
    }
    struct stat status{};
    if (::lstat(invocation.runtime_dir.c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
        status.st_uid == ::geteuid()) {
        dirs.push_back(invocation.runtime_dir);
    }
    return dirs;
}

/// What to add to "can't reach kool_spin" when an engine runs without it
/// (specs/supervisor-crash/spec.md requirement 18, specs/state-folder/spec.md
/// requirement 14); otherwise nothing. Only reads lock files.
std::string engine_hint(const Invocation& invocation) {
    bool running = false;
    std::optional<pid_t> pid;
    for (const std::filesystem::path& dir : lock_dirs(invocation)) {
        const auto supervisor = ipc::probe_lock(dir, ipc::kSupervisorLockName);
        if (!supervisor || supervisor->held) {
            return {};
        }
        const auto engine = ipc::probe_lock(dir, ipc::kEngineLockName);
        if (engine && engine->held) {
            running = true;
            pid = pid ? pid : engine->pid;
        }
    }
    if (!running) {
        return {};
    }
    const std::string process = pid ? " (process " + std::to_string(*pid) + ")" : std::string{};
    return "; an engine is running without kool_spin" + process + ": run kool_spin to stop it";
}

/// Connects and does the one command: run()'s worker thread. `stop` is set
/// when `status --watch` gets SIGINT or SIGTERM.
int work(const Invocation& invocation, const std::atomic<bool>& stop, std::ostream& out,
         std::ostream& err) {
    client::Result<Session> session = Session::connect(invocation.name, invocation.runtime_dir);
    if (!session) {
        if (session.error().kind == ErrorKind::NotReachable) {
            err << "ks-play: can't reach kool_spin in " << invocation.runtime_dir.string() << ": "
                << session.error().text << engine_hint(invocation) << '\n';
            return kExitUnreachable;
        }
        return report(err, session.error());
    }

    return std::visit(
        [&](const auto& request) -> int {
            using T = std::decay_t<decltype(request)>;
            if constexpr (std::is_same_v<T, Load>) {
                return do_load(*session, request, out, err);
            } else if constexpr (std::is_same_v<T, Unload>) {
                return do_unload(*session, request, out, err);
            } else if constexpr (std::is_same_v<T, Control>) {
                return do_control(*session, request, out, err);
            } else if constexpr (std::is_same_v<T, Status>) {
                return do_status(*session, request, stop, out, err);
            } else if constexpr (std::is_same_v<T, Health>) {
                return do_health(*session, out, err);
            } else if constexpr (std::is_same_v<T, Stop>) {
                return do_stop(*session, out, err);
            } else if constexpr (std::is_same_v<T, Ping>) {
                return do_ping(*session, out, err);
            } else {
                static_assert(std::is_same_v<T, Help>);
                out << usage();
                return finish(out, err);
            }
        },
        invocation.request);
}

/// The worker's private signal: it wakes the waiting thread when the work is
/// done.
constexpr int kWakeSignal = SIGUSR1;

/// SIGINT, SIGTERM, and kWakeSignal: blocked in every thread while run()
/// works, and taken with sigwait(). No handler is installed, so
/// RealtimeSanitizer never sees one interrupt KS_NONBLOCKING code (CLAUDE.md;
/// plan, "Decisions").
sigset_t waited_signals() noexcept {
    sigset_t set{};
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, kWakeSignal);
    return set;
}

/// Takes every waited signal still pending, such as a SIGINT that came after
/// the work was done, so that none ends the process once the mask is
/// restored.
void drain_waited_signals(const sigset_t& set) noexcept {
    for (;;) {
        sigset_t pending{};
        sigemptyset(&pending);
        if (sigpending(&pending) != 0) {
            return;
        }
        bool took = false;
        for (const int signal : {SIGINT, SIGTERM, kWakeSignal}) {
            if (sigismember(&set, signal) == 1 && sigismember(&pending, signal) == 1) {
                sigset_t one{};
                sigemptyset(&one);
                sigaddset(&one, signal);
                int taken = 0;
                took = sigwait(&one, &taken) == 0 || took;
            }
        }
        if (!took) {
            return;
        }
    }
}

/// Ignores SIGPIPE while it lives (requirement 29: a closed standard output
/// is a failed write, status 1), then puts back what was there.
class SigpipeIgnored {
public:
    SigpipeIgnored() noexcept : previous_{std::signal(SIGPIPE, SIG_IGN)} {}
    SigpipeIgnored(const SigpipeIgnored&) = delete;
    SigpipeIgnored& operator=(const SigpipeIgnored&) = delete;
    SigpipeIgnored(SigpipeIgnored&&) = delete;
    SigpipeIgnored& operator=(SigpipeIgnored&&) = delete;
    ~SigpipeIgnored() {
        if (previous_ != SIG_ERR) {
            std::signal(SIGPIPE, previous_);
        }
    }

private:
    using Handler = void (*)(int);
    Handler previous_;
};

} // namespace

int run(const Invocation& invocation, std::ostream& out, std::ostream& err) {
    if (std::holds_alternative<Help>(invocation.request)) {
        out << usage();
        return finish(out, err);
    }
    if (const Load* load = std::get_if<Load>(&invocation.request);
        load != nullptr && !std::filesystem::path{load->path}.is_absolute()) {
        // Only when the working directory couldn't be read (main_entry()).
        err << "ks-play: can't make the path absolute: " << load->path << '\n';
        return kExitFailed;
    }

    const SigpipeIgnored sigpipe_ignored;
    const sigset_t signals = waited_signals();
    sigset_t previous{};
    if (const int blocked = pthread_sigmask(SIG_BLOCK, &signals, &previous); blocked != 0) {
        err << "ks-play: can't block signals: " << std::generic_category().message(blocked) << '\n';
        return kExitFailed;
    }
    const Status* const status = std::get_if<Status>(&invocation.request);
    const bool watch = status != nullptr && status->watch;

    // The worker inherits the mask. When it finishes, it sets `done` and
    // wakes this thread with kWakeSignal.
    std::atomic<bool> stop{false};
    std::atomic<bool> done{false};
    int result = kExitFailed;
    const pthread_t waiting = pthread_self();
    std::thread worker;
    try {
        worker = std::thread{[&] {
            result = work(invocation, stop, out, err);
            done.store(true, std::memory_order_release);
            pthread_kill(waiting, kWakeSignal);
        }};
    } catch (const std::system_error& error) {
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        err << "ks-play: can't start a thread: " << error.what() << '\n';
        return kExitFailed;
    }

    for (;;) {
        int signal = 0;
        if (sigwait(&signals, &signal) != 0 || done.load(std::memory_order_acquire)) {
            break;
        }
        if (signal == kWakeSignal) {
            continue; // Not the worker's: it sets `done` first.
        }
        if (!watch) {
            // Ending the process closes the connection; no destructor races
            // the worker (requirement 29).
            std::_Exit(signal == SIGINT ? kExitInterrupted : kExitTerminated);
        }
        stop.store(true, std::memory_order_release);
    }
    worker.join();
    drain_waited_signals(signals);
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    return stop.load(std::memory_order_acquire) ? kExitOk : result;
}

int main_entry(int argc, const char* const* argv, std::ostream& out, std::ostream& err) {
    std::vector<std::string_view> args;
    if (argc > 1 && argv != nullptr) {
        const std::span<const char* const> all{argv, static_cast<std::size_t>(argc)};
        for (const char* arg : all.subspan(1)) {
            args.emplace_back(arg != nullptr ? std::string_view{arg} : std::string_view{});
        }
    }

    // A working directory that can't be read leaves relative paths relative,
    // and run() refuses them; every other command works without one.
    std::error_code cwd_error;
    std::filesystem::path cwd = std::filesystem::current_path(cwd_error);
    if (cwd_error) {
        cwd.clear();
    }

    const std::expected<Invocation, std::string> invocation =
        parse(args, cwd, ipc::default_runtime_directory_path());
    if (!invocation) {
        err << "ks-play: " << invocation.error() << " (see ks-play --help)\n";
        return kExitUsage;
    }
    return run(*invocation, out, err);
}

} // namespace ks::play_app
