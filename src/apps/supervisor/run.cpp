// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/supervisor/run.h>

#include <ks/ipc/command.h>
#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/fd_passing.h>
#include <ks/ipc/frame.h>
#include <ks/ipc/listener.h>
#include <ks/ipc/log.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/region.h>
#include <ks/ipc/runtime_dir.h>
#include <ks/ipc/segment.h>
#include <ks/ipc/state.h>
#include <ks/supervisor/log_dir.h>
#include <ks/supervisor/log_file.h>
#include <ks/supervisor/options.h>
#include <ks/supervisor/spawn.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace ks::supervisor {

namespace {

using Clock = std::chrono::steady_clock;

// ---- Signals, through a self-pipe (requirement 16) -----------------------------

/// The signals the supervisor handles.
constexpr std::array<int, 4> kHandledSignals{SIGCHLD, SIGINT, SIGTERM, SIGHUP};

/// The write end of the self-pipe. A signal handler can reach only a global.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): see above
std::atomic<int> signal_pipe{-1};
static_assert(std::atomic<int>::is_always_lock_free, "a signal handler reads it");

extern "C" void on_signal(int signal) {
    const int saved = errno;
    const auto byte = static_cast<unsigned char>(signal);
    static_cast<void>(::write(signal_pipe.load(std::memory_order_relaxed), &byte, 1));
    errno = saved;
}

/// Sets O_NONBLOCK and FD_CLOEXEC on `fd`. False if either fails.
bool set_nonblocking_cloexec(int fd) noexcept {
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg): fcntl() is variadic
    const int status = ::fcntl(fd, F_GETFL);
    const int flags = ::fcntl(fd, F_GETFD);
    return status != -1 && flags != -1 && ::fcntl(fd, F_SETFL, status | O_NONBLOCK) != -1 &&
           ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != -1;
    // NOLINTEND(cppcoreguidelines-pro-type-vararg)
}

/// A self-pipe that handlers for some signals write the signal's number to.
class SignalPipe {
public:
    /// Creates the pipe and installs handlers for `signals`.
    static std::expected<SignalPipe, std::error_code> install(std::span<const int> signals) {
        std::array<int, 2> ends{-1, -1};
        if (::pipe(ends.data()) != 0) {
            return std::unexpected{std::error_code{errno, std::system_category()}};
        }
        SignalPipe pipe{ipc::Fd{ends[0]}, ipc::Fd{ends[1]}};
        if (!set_nonblocking_cloexec(ends[0]) || !set_nonblocking_cloexec(ends[1])) {
            return std::unexpected{std::error_code{errno, std::system_category()}};
        }
        signal_pipe.store(ends[1], std::memory_order_relaxed);
        for (const int signal : signals) {
            struct sigaction action{};
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): sigaction's handler is a
            // union member
            action.sa_handler = &on_signal;
            sigemptyset(&action.sa_mask);
            action.sa_flags = signal == SIGCHLD ? SA_NOCLDSTOP : 0;
            if (::sigaction(signal, &action, nullptr) != 0) {
                return std::unexpected{std::error_code{errno, std::system_category()}};
            }
        }
        return pipe;
    }

    [[nodiscard]] int read_end() const noexcept { return read_.get(); }

    /// The signals that have arrived since the last call, in order.
    [[nodiscard]] std::vector<int> take() const {
        std::vector<int> signals;
        std::array<unsigned char, 64> bytes{};
        for (;;) {
            const ssize_t got = ::read(read_.get(), bytes.data(), bytes.size());
            if (got <= 0) {
                if (got == -1 && errno == EINTR) {
                    continue;
                }
                return signals;
            }
            for (const unsigned char byte : std::span{bytes}.first(static_cast<std::size_t>(got))) {
                signals.push_back(byte);
            }
        }
    }

private:
    SignalPipe(ipc::Fd read, ipc::Fd write) noexcept
        : read_{std::move(read)}, write_{std::move(write)} {}

    ipc::Fd read_;
    ipc::Fd write_;
};

/// Blocks the handled signals in this thread while it lives, then restores
/// the thread's mask. A signal that arrives meanwhile is handled then.
///
/// The shared-memory primitives are KS_NONBLOCKING, so RealtimeSanitizer
/// counts a signal handler that interrupts one as real-time code, and
/// on_signal()'s write() as a violation. The supervisor isn't real-time,
/// but it reads shared memory only inside one of these, so no handler runs
/// there.
class SignalsBlocked {
public:
    SignalsBlocked() noexcept {
        sigset_t handled{};
        static_cast<void>(sigemptyset(&handled));
        for (const int signal : kHandledSignals) {
            static_cast<void>(sigaddset(&handled, signal));
        }
        blocked_ = ::pthread_sigmask(SIG_BLOCK, &handled, &previous_) == 0;
    }

    SignalsBlocked(const SignalsBlocked&) = delete;
    SignalsBlocked& operator=(const SignalsBlocked&) = delete;
    SignalsBlocked(SignalsBlocked&&) = delete;
    SignalsBlocked& operator=(SignalsBlocked&&) = delete;

    ~SignalsBlocked() {
        if (blocked_) {
            static_cast<void>(::pthread_sigmask(SIG_SETMASK, &previous_, nullptr));
        }
    }

private:
    sigset_t previous_{};
    bool blocked_ = false;
};

// ---- Small helpers ----------------------------------------------------------

/// How a process ended, from its wait status, for a report.
std::string how_it_ended(int status) {
    if (WIFEXITED(status)) {
        return std::format("exited with status {}", WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        return std::format("was killed by signal {}", WTERMSIG(status));
    }
    return "ended";
}

/// The milliseconds from now until `deadline`, rounded up, for poll(); -1 for
/// no deadline.
int poll_timeout(std::optional<Clock::time_point> deadline) {
    if (!deadline) {
        return -1;
    }
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(*deadline - Clock::now());
    return static_cast<int>(std::clamp<std::int64_t>(left.count(), 0, 60'000));
}

/// std::chrono::steady_clock now, in nanoseconds: a log line's time.
std::uint64_t steady_now_ns() {
    const auto since =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch());
    return static_cast<std::uint64_t>(since.count());
}

/// The text of an Error message.
ipc::Message error_message(std::uint32_t request_id, ipc::ErrorCode code, std::string text) {
    return ipc::Message{.request_id = request_id,
                        .body = ipc::msg::Error{.code = code, .text = std::move(text)}};
}

// ---- The supervisor -----------------------------------------------------------

/// What the supervisor is doing.
enum class Phase : std::uint8_t {
    Starting,    ///< Waiting for Ready.
    Running,     ///< Listening for clients.
    Stopping,    ///< Asked the engine to stop; waiting for it to exit.
    Terminating, ///< Sent the engine SIGTERM after its connection failed.
};

/// A client's command ring, once the engine has attached it
/// (specs/client-rings/spec.md requirement 10).
struct Ring {
    std::uint32_t slot = 0;
    /// The supervisor's own descriptor of the segment.
    ipc::Fd segment;
};

/// One client's connection.
struct Client {
    ipc::Connection connection;
    /// Tells clients apart for answers that come later, from the engine.
    std::uint64_t id = 0;
    bool welcomed = false;
    std::string name;
    /// The request IDs of its Stops, to answer with Done.
    std::vector<std::uint32_t> stops;
    /// Its ring, once attached.
    std::unique_ptr<Ring> ring;
    /// An Attach for its ring is waiting for the engine's answer.
    bool opening = false;
    /// Set once it's to be closed.
    bool dropped = false;
};

/// An Attach sent to the engine, waiting for its answer
/// (specs/client-rings/spec.md requirement 10).
struct PendingAttach {
    std::uint32_t request = 0;        ///< The Attach's request ID.
    std::uint64_t client = 0;         ///< Client::id.
    std::uint32_t client_request = 0; ///< The client's OpenRing's request ID.
    ipc::Fd segment;
};

/// A Detach sent to the engine, waiting for its answer. The supervisor keeps
/// its descriptor of the segment until then (requirement 11).
struct PendingDetach {
    std::uint32_t request = 0; ///< The Detach's request ID.
    std::uint32_t slot = 0;
    ipc::Fd segment;
};

class Supervisor {
public:
    Supervisor(const Options& options, std::ostream& err) : options_{&options}, err_{&err} {}

    int run() {
        if (const int status = start(); status != kExitOk) {
            return status;
        }
        for (;;) {
            if (const std::optional<int> status = step()) {
                return *status;
            }
        }
    }

private:
    /// Every report goes through here (specs/logs/spec.md requirements 13
    /// and 15): to kool_spin.log, and, for a warning or an error, to standard
    /// error too. Without a log file, every line goes to standard error.
    ///
    /// A kool_spin.log that fails is reported once, on standard error, and
    /// every later line goes there instead (requirement 13).
    void report(ipc::LogLevel level, std::string_view line) {
        bool in_file = false;
        if (log_ && !log_->error()) {
            static_cast<void>(log_->write(steady_now_ns(), level, line));
            if (const std::optional<std::error_code> failed = log_->error()) {
                *err_ << "kool_spin: "
                      << std::format("kool_spin.log failed: {}; log lines go to standard error",
                                     failed->message())
                      << '\n';
            } else {
                in_file = true;
            }
        }
        if (!in_file || level != ipc::LogLevel::Info) {
            *err_ << "kool_spin: " << line << '\n' << std::flush;
        }
    }
    void info(std::string_view line) { report(ipc::LogLevel::Info, line); }
    void warning(std::string_view line) { report(ipc::LogLevel::Warning, line); }
    void error(std::string_view line) { report(ipc::LogLevel::Error, line); }

    /// Requirement 15, steps 1 to 5. Returns kExitOk, or reports and returns
    /// kExitStartFailed.
    int start();

    /// Reads the session's clock, and creates the log folder, the session
    /// folder, and kool_spin.log (specs/logs/spec.md requirement 12). If one
    /// can't be created, warns once, and the supervisor runs without log
    /// files (requirement 13).
    void open_logs();
    /// Creates ks-engine.log in the session folder, if there is one.
    void open_engine_log();

    /// Writes every record in the engine's log ring to ks-engine.log, and
    /// what was dropped, until the ring is empty (specs/logs/spec.md
    /// requirement 14). Reads at most a ring's worth, so an engine that
    /// writes without pause can't hold up the poll loop.
    void drain_engine_log();
    /// One of the engine's records, in ks-engine.log or on standard error.
    void engine_line(const ipc::LogRecord& record);
    /// A line of the supervisor's own about the engine's log, in
    /// ks-engine.log or on standard error.
    void engine_note(ipc::LogLevel level, std::string_view text);
    /// True if ks-engine.log is there and hasn't failed.
    [[nodiscard]] bool engine_file() const { return engine_log_ && !engine_log_->error(); }
    /// After a write to ks-engine.log: reports it, once, if it has just
    /// failed. Returns whether it still works.
    bool engine_file_ok();

    /// One pass of the poll loop. Returns the exit status once the
    /// supervisor is done.
    std::optional<int> step();

    std::optional<int> handle_signals();
    std::optional<int> engine_exited(int status);
    void handle_engine(short events);
    void handle_engine_message(const ipc::Message& message);
    void engine_failed(std::string_view why);
    void send_to_engine(const ipc::Message& message, std::span<const int> fds = {});
    /// A request ID for a request to the engine; never 0.
    std::uint32_t next_request();
    /// The client with Client::id `id`, or null if it has gone or been
    /// dropped.
    Client* find_client(std::uint64_t id);
    /// Handles a client's OpenRing (client-rings requirement 10).
    void open_ring(Client& client, std::uint32_t id);
    /// Handles the engine's answers to Attach and Detach. Each returns false
    /// if `id` answers no such request.
    bool ring_attached(std::uint32_t id, std::uint32_t slot);
    bool ring_detached(std::uint32_t id);
    bool attach_refused(std::uint32_t id, const ipc::msg::Error& error);
    bool detach_refused(std::uint32_t id, const ipc::msg::Error& error);
    /// Asks the engine to detach the ring in `slot` (requirement 11).
    void detach_ring(std::uint32_t slot, ipc::Fd segment);
    void accept_clients();
    void handle_client(Client& client, short events);
    void handle_client_message(Client& client, const ipc::Message& message);
    void send_to_client(Client& client, const ipc::Message& message, std::span<const int> fds = {});
    void refuse_and_drop(Client& client, std::uint32_t request_id, std::string why);
    /// Drops `client`, and reports it at `level`: info for a client that
    /// closed its connection, and a warning for anything else.
    void drop(Client& client, std::string_view why, ipc::LogLevel level = ipc::LogLevel::Warning);
    void begin_stop(std::string_view why);
    std::optional<int> deadline_passed();
    void kill_engine();
    void flush_clients();

    const Options* options_;
    std::ostream* err_;

    std::filesystem::path dir_;
    std::unique_ptr<ipc::LockFile> lock_;
    std::unique_ptr<ipc::SegmentFactory> factory_;
    ipc::Fd state_;
    std::unique_ptr<SignalPipe> signals_;

    /// Read once, as the session starts: it converts every line's time.
    ipc::LogClock clock_;
    /// The session folder; empty if there are no log files.
    std::filesystem::path session_;
    /// kool_spin.log; null if there are no log files.
    std::unique_ptr<LogFile> log_;
    /// The engine's log segment, kept until the supervisor exits.
    ipc::Fd engine_log_segment_;
    /// The supervisor's mapping of the engine's log segment, kept until it
    /// exits, and the reader of its ring.
    std::unique_ptr<ipc::Region> engine_log_region_;
    std::unique_ptr<ipc::LogReader> engine_log_reader_;
    /// ks-engine.log; null if it couldn't be created.
    std::unique_ptr<LogFile> engine_log_;

    pid_t engine_pid_ = 0;
    std::unique_ptr<ipc::Connection> engine_;
    std::uint32_t next_request_ = 1;

    std::unique_ptr<ipc::Listener> listener_;
    std::vector<Client> clients_;
    std::uint64_t next_client_ = 1;
    std::vector<PendingAttach> attaches_;
    std::vector<PendingDetach> detaches_;

    Phase phase_ = Phase::Starting;
    std::optional<Clock::time_point> deadline_;
    /// Set when the supervisor must exit with this status whatever the engine
    /// does: it's stopping the engine because it couldn't start.
    std::optional<int> forced_status_;
};

int Supervisor::start() {
    dir_ = options_->runtime_dir.value_or(ipc::default_runtime_directory_path());
    if (const auto opened = ipc::open_runtime_directory(dir_); !opened) {
        error(std::format("can't use the runtime directory {}: {}", dir_.string(),
                          opened.error().message()));
        return kExitStartFailed;
    }

    std::expected<ipc::LockFile, ipc::LockFile::Failure> lock =
        ipc::LockFile::acquire(dir_, ipc::kSupervisorLockName);
    if (!lock) {
        if (lock.error().holder) {
            error(std::format("another supervisor is running (process {})",
                              lock.error().holder.value_or(0)));
        } else {
            error(std::format("can't take the supervisor lock: {}", lock.error().error.message()));
        }
        return kExitStartFailed;
    }
    lock_ = std::make_unique<ipc::LockFile>(std::move(*lock));
    open_logs();

    const std::expected<ipc::LockState, std::error_code> engine_lock =
        ipc::probe_lock(dir_, ipc::kEngineLockName);
    if (!engine_lock) {
        error(std::format("can't check the engine lock: {}", engine_lock.error().message()));
        return kExitStartFailed;
    }
    if (engine_lock->held) {
        error(engine_lock->pid ? std::format("an engine from an earlier session is still running "
                                             "(process {})",
                                             *engine_lock->pid)
                               : std::string{"an engine from an earlier session is still running"});
        return kExitStartFailed;
    }

    std::filesystem::path engine;
    if (options_->engine) {
        engine = *options_->engine;
    } else if (std::expected<std::filesystem::path, std::error_code> found =
                   default_engine_path()) {
        engine = std::move(*found);
    } else {
        error(std::format("can't find ks-engine: {}", found.error().message()));
        return kExitStartFailed;
    }

    std::expected<ipc::SegmentFactory, std::error_code> factory = ipc::SegmentFactory::open(dir_);
    if (!factory) {
        error(std::format("can't create segments: {}", factory.error().message()));
        return kExitStartFailed;
    }
    factory_ = std::make_unique<ipc::SegmentFactory>(std::move(*factory));
    std::expected<ipc::Fd, std::error_code> state =
        factory_->create(ipc::SegmentKind::EngineState, ipc::kEngineStateLayoutVersion,
                         ipc::kEngineStateSegmentSize);
    if (!state) {
        error(std::format("can't create the engine-state segment: {}", state.error().message()));
        return kExitStartFailed;
    }
    state_ = std::move(*state);
    std::expected<ipc::Fd, std::error_code> engine_log =
        factory_->create(ipc::SegmentKind::Log, ipc::kLogLayoutVersion, ipc::kLogSegmentSize);
    if (!engine_log) {
        error(
            std::format("can't create the engine's log segment: {}", engine_log.error().message()));
        return kExitStartFailed;
    }
    engine_log_segment_ = std::move(*engine_log);
    std::expected<ipc::Region, ipc::MapFailure> engine_log_region =
        ipc::Region::map(engine_log_segment_.get(), ipc::SegmentKind::Log, ipc::kLogLayoutVersion,
                         ipc::Access::ReadWrite);
    if (!engine_log_region) {
        error("can't map the engine's log segment");
        return kExitStartFailed;
    }
    engine_log_region_ = std::make_unique<ipc::Region>(std::move(*engine_log_region));
    // at() checks against the size of the supervisor's own mapping.
    auto* const engine_log_ring = engine_log_region_->at<ipc::LogSegment>(ipc::kSegmentHeaderSize);
    if (engine_log_ring == nullptr) {
        error("the engine's log segment is too small");
        return kExitStartFailed;
    }
    engine_log_reader_ = std::make_unique<ipc::LogReader>(*engine_log_ring);

    std::expected<SignalPipe, std::error_code> signals = SignalPipe::install(kHandledSignals);
    if (!signals) {
        error(std::format("can't handle signals: {}", signals.error().message()));
        return kExitStartFailed;
    }
    signals_ = std::make_unique<SignalPipe>(std::move(*signals));

    std::expected<std::pair<ipc::Fd, ipc::Fd>, std::error_code> pair = ipc::socket_pair();
    if (!pair || !set_nonblocking_cloexec(pair->first.get())) {
        error("can't create the engine's socket");
        return kExitStartFailed;
    }
    std::vector<std::string> args{"--supervisor-fd", "3", "--state-fd",    "4",
                                  "--log-fd",        "5", "--runtime-dir", dir_.string()};
    args.insert(args.end(), options_->engine_args.begin(), options_->engine_args.end());
    const std::array<Inherit, 3> fds{Inherit{.parent = pair->second.get(), .child = 3},
                                     Inherit{.parent = state_.get(), .child = 4},
                                     Inherit{.parent = engine_log_segment_.get(), .child = 5}};
    open_engine_log();
    const std::expected<pid_t, std::error_code> pid = spawn(engine, args, fds);
    if (!pid) {
        error(std::format("can't start {}: {}", engine.string(), pid.error().message()));
        return kExitStartFailed;
    }
    engine_pid_ = *pid;
    engine_ = std::make_unique<ipc::Connection>(std::move(pair->first));
    info(std::format("started ks-engine (process {})", engine_pid_));
    deadline_ = Clock::now() + kEngineReadyTimeout;
    return kExitOk;
}

void Supervisor::open_logs() {
    clock_ = ipc::LogClock::now();
    constexpr std::string_view kInstead = "log lines go to standard error";
    const std::optional<std::filesystem::path> dir =
        options_->log_dir ? options_->log_dir : default_log_dir();
    if (!dir) {
        warning(std::format("no log folder: HOME isn't set to an absolute path; {}", kInstead));
        return;
    }
    if (const std::error_code failed = create_log_dir(*dir)) {
        warning(std::format("can't create the log folder {}: {}; {}", dir->string(),
                            failed.message(), kInstead));
        return;
    }
    remove_old_sessions(*dir);
    const std::optional<std::string> name = session_name(clock_, ::getpid());
    if (!name) {
        warning(std::format("can't name a session folder in {}: the clock is out of range; {}",
                            dir->string(), kInstead));
        return;
    }
    const std::filesystem::path session = *dir / *name;
    if (::mkdir(session.c_str(), 0700) != 0) {
        warning(std::format("can't create the session folder {}: {}; {}", session.string(),
                            std::error_code{errno, std::system_category()}.message(), kInstead));
        return;
    }
    const std::filesystem::path file = session / "kool_spin.log";
    std::expected<LogFile, std::error_code> log = LogFile::create(file, "kool_spin", clock_);
    if (!log) {
        warning(
            std::format("can't create {}: {}; {}", file.string(), log.error().message(), kInstead));
        return;
    }
    session_ = session;
    log_ = std::make_unique<LogFile>(std::move(*log));
}

void Supervisor::open_engine_log() {
    if (session_.empty()) {
        return;
    }
    const std::filesystem::path file = session_ / "ks-engine.log";
    std::expected<LogFile, std::error_code> log = LogFile::create(file, "ks-engine", clock_);
    if (!log) {
        warning(std::format("can't create {}: {}; the engine's lines go to standard error",
                            file.string(), log.error().message()));
        return;
    }
    engine_log_ = std::make_unique<LogFile>(std::move(*log));
}

void Supervisor::drain_engine_log() {
    // Every call into the reader runs inside this (see SignalsBlocked).
    const SignalsBlocked blocked;
    if (!engine_log_reader_ || engine_log_reader_->corrupt()) {
        return;
    }
    for (std::size_t i = 0; i < ipc::kLogRingCapacity; ++i) {
        const std::optional<ipc::LogRecord> record = engine_log_reader_->next();
        if (!record) {
            break;
        }
        engine_line(*record);
    }
    const std::uint64_t dropped =
        engine_log_reader_->corrupt() ? 0 : engine_log_reader_->take_dropped();
    if (engine_log_reader_->corrupt()) {
        constexpr std::string_view kCorrupt =
            "the engine's log ring is corrupt; it won't be read again";
        engine_note(ipc::LogLevel::Error, kCorrupt);
        error(kCorrupt);
        return;
    }
    if (dropped > 0) {
        engine_note(ipc::LogLevel::Warning,
                    std::format("{} {} dropped because the engine's log ring was full", dropped,
                                dropped == 1 ? "line was" : "lines were"));
    }
}

void Supervisor::engine_line(const ipc::LogRecord& record) {
    if (engine_file()) {
        static_cast<void>(engine_log_->write(record));
        if (engine_file_ok()) {
            return;
        }
    }
    *err_ << "ks-engine: " << ipc::format_log_record(clock_, record) << '\n' << std::flush;
}

void Supervisor::engine_note(ipc::LogLevel level, std::string_view text) {
    const std::uint64_t now = steady_now_ns();
    if (engine_file()) {
        static_cast<void>(engine_log_->write(now, level, text));
        if (engine_file_ok()) {
            return;
        }
    }
    *err_ << "ks-engine: " << ipc::format_log_line(clock_, now, level, text) << '\n' << std::flush;
}

bool Supervisor::engine_file_ok() {
    const std::optional<std::error_code> failed = engine_log_->error();
    if (!failed) {
        return true;
    }
    error(std::format("ks-engine.log failed: {}; the engine's lines go to standard error",
                      failed->message()));
    return false;
}

std::optional<int> Supervisor::step() {
    // The signal pipe, the engine, the listener, then each client.
    std::vector<pollfd> polled;
    polled.push_back({.fd = signals_->read_end(), .events = POLLIN, .revents = 0});
    const auto out_if = [](const ipc::Connection& c) -> short {
        return c.has_queued() ? static_cast<short>(POLLIN | POLLOUT) : static_cast<short>(POLLIN);
    };
    polled.push_back({.fd = engine_ ? engine_->socket() : -1,
                      .events = engine_ ? out_if(*engine_) : static_cast<short>(0),
                      .revents = 0});
    polled.push_back({.fd = listener_ ? listener_->socket() : -1, .events = POLLIN, .revents = 0});
    for (const Client& client : clients_) {
        polled.push_back(
            {.fd = client.connection.socket(), .events = out_if(client.connection), .revents = 0});
    }
    // While the engine runs, its log ring is read at least every
    // kLogDrainInterval (specs/logs/spec.md requirement 14).
    int timeout = poll_timeout(deadline_);
    if (engine_pid_ > 0) {
        const auto drain = static_cast<int>(kLogDrainInterval.count());
        timeout = timeout == -1 ? drain : std::min(timeout, drain);
    }
    const int ready = ::poll(polled.data(), static_cast<nfds_t>(polled.size()), timeout);
    if (ready == -1 && errno != EINTR) {
        error(std::format("poll failed: {}",
                          std::error_code{errno, std::system_category()}.message()));
    }
    drain_engine_log();

    if (const std::optional<int> status = handle_signals()) {
        return status;
    }
    if (polled[1].revents != 0 && engine_) {
        handle_engine(polled[1].revents);
    }
    if (polled[2].revents != 0 && listener_) {
        accept_clients();
    }
    // Clients accepted just now weren't polled: they come after the others.
    for (std::size_t i = 3; i < polled.size(); ++i) {
        if (polled[i].revents != 0) {
            handle_client(clients_[i - 3], polled[i].revents);
        }
    }
    std::erase_if(clients_, [](const Client& client) { return client.dropped; });
    return deadline_passed();
}

std::optional<int> Supervisor::handle_signals() {
    for (const int signal : signals_->take()) {
        if (signal != SIGCHLD) {
            begin_stop(std::format("got signal {}", signal));
            continue;
        }
        int status = 0;
        pid_t reaped = ::waitpid(engine_pid_, &status, WNOHANG);
        while (reaped == -1 && errno == EINTR) {
            reaped = ::waitpid(engine_pid_, &status, WNOHANG);
        }
        if (reaped == engine_pid_) {
            return engine_exited(status);
        }
    }
    return std::nullopt;
}

std::optional<int> Supervisor::engine_exited(int status) {
    engine_pid_ = 0;
    // Its last lines, before how it ended (requirement 14).
    drain_engine_log();
    const std::string how = how_it_ended(status);
    const bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (forced_status_) {
        // The supervisor is stopping it after a failure it has reported.
        report(clean ? ipc::LogLevel::Info : ipc::LogLevel::Error,
               std::format("ks-engine {}", how));
        return forced_status_;
    }
    switch (phase_) {
    case Phase::Starting:
        error(std::format("ks-engine {} before it was ready", how));
        return kExitStartFailed;
    case Phase::Running:
    case Phase::Terminating:
        error(std::format("ks-engine {}", how));
        return kExitEngineFailed;
    case Phase::Stopping:
        break;
    }
    if (!clean) {
        error(std::format("ks-engine {} while stopping", how));
        return kExitEngineFailed;
    }
    info("ks-engine stopped");
    for (Client& client : clients_) {
        for (const std::uint32_t id : std::exchange(client.stops, {})) {
            send_to_client(client, ipc::Message{.request_id = id, .body = ipc::msg::Done{}});
        }
    }
    flush_clients();
    return kExitOk;
}

void Supervisor::handle_engine(short events) {
    if ((events & POLLOUT) != 0 && !engine_->flush()) {
        engine_failed("couldn't be written to");
        return;
    }
    for (;;) {
        std::expected<std::optional<ipc::ReceivedFrame>, ipc::ConnectionFailure> received =
            engine_->receive();
        if (!received) {
            if (received.error().error == ipc::ConnectionError::BadFrame) {
                send_to_engine(error_message(0, ipc::ErrorCode::BadMessage, "a bad frame"));
                engine_failed("sent a bad frame");
            } else {
                engine_failed("closed");
            }
            return;
        }
        const std::optional<ipc::ReceivedFrame>& frame = *received;
        if (!frame) {
            return;
        }
        const std::expected<ipc::Message, ipc::MessageError> message = ipc::decode(frame->frame);
        if (!message) {
            send_to_engine(error_message(frame->frame.request_id, ipc::ErrorCode::BadMessage,
                                         "a message that doesn't decode"));
            engine_failed("sent a message that doesn't decode");
            return;
        }
        handle_engine_message(*message);
        if (!engine_) {
            return;
        }
    }
}

void Supervisor::handle_engine_message(const ipc::Message& message) {
    const std::uint32_t id = message.request_id;
    if (std::holds_alternative<ipc::msg::Ready>(message.body) && phase_ == Phase::Starting) {
        info("ks-engine is ready");
        std::expected<ipc::Listener, std::error_code> listener = ipc::Listener::open(dir_);
        if (!listener) {
            error(std::format("can't open the supervisor socket: {}", listener.error().message()));
            forced_status_ = kExitStartFailed;
            begin_stop("it couldn't start");
            return;
        }
        listener_ = std::make_unique<ipc::Listener>(std::move(*listener));
        phase_ = Phase::Running;
        deadline_.reset();
        return;
    }
    if (std::holds_alternative<ipc::msg::Ping>(message.body)) {
        send_to_engine(ipc::Message{.request_id = id, .body = ipc::msg::Pong{}});
    } else if (const auto* const error = std::get_if<ipc::msg::Error>(&message.body)) {
        if (!attach_refused(id, *error) && !detach_refused(id, *error)) {
            warning(std::format("ks-engine reported an error: {}", error->text));
        }
    } else if (const auto* const attached = std::get_if<ipc::msg::Attached>(&message.body)) {
        if (!ring_attached(id, attached->slot)) {
            send_to_engine(error_message(id, ipc::ErrorCode::NotSupported, "answers no Attach"));
        }
    } else if (std::holds_alternative<ipc::msg::Detached>(message.body)) {
        if (!ring_detached(id)) {
            send_to_engine(error_message(id, ipc::ErrorCode::NotSupported, "answers no Detach"));
        }
    } else if (!std::holds_alternative<ipc::msg::Pong>(message.body) &&
               !std::holds_alternative<ipc::msg::Done>(message.body)) {
        send_to_engine(error_message(id, ipc::ErrorCode::NotSupported, "not from the engine"));
    }
}

void Supervisor::send_to_engine(const ipc::Message& message, std::span<const int> fds) {
    if (!engine_) {
        return;
    }
    const std::expected<ipc::Frame, ipc::MessageError> frame = ipc::encode(message);
    if (!frame || !engine_->send(*frame, fds) || !engine_->flush()) {
        engine_failed("couldn't be written to");
    }
}

std::uint32_t Supervisor::next_request() {
    if (next_request_ == 0) {
        next_request_ = 1;
    }
    return next_request_++;
}

Client* Supervisor::find_client(std::uint64_t id) {
    for (Client& client : clients_) {
        if (client.id == id) {
            return client.dropped ? nullptr : &client;
        }
    }
    return nullptr;
}

void Supervisor::open_ring(Client& client, std::uint32_t id) {
    info(std::format("client '{}' asked for a ring", client.name));
    if (client.ring || client.opening) {
        warning(std::format("refused client '{}' a second ring", client.name));
        send_to_client(client, error_message(id, ipc::ErrorCode::NotSupported,
                                             "a client has one ring at a time"));
        return;
    }
    if (phase_ != Phase::Running) {
        warning(std::format("refused client '{}' a ring: stopping", client.name));
        send_to_client(client, error_message(id, ipc::ErrorCode::NotSupported, "stopping"));
        return;
    }
    std::expected<ipc::Fd, std::error_code> segment =
        factory_->create(ipc::SegmentKind::CommandRing, ipc::kCommandRingLayoutVersion,
                         ipc::kCommandRingSegmentSize);
    if (!segment) {
        const std::string why =
            std::format("can't create a ring segment: {}", segment.error().message());
        warning(std::format("refused client '{}' a ring: {}", client.name, why));
        send_to_client(client, error_message(id, ipc::ErrorCode::Unavailable, why));
        return;
    }
    const std::uint32_t request = next_request();
    const std::array<int, 1> fds{segment->get()};
    client.opening = true;
    attaches_.push_back(PendingAttach{.request = request,
                                      .client = client.id,
                                      .client_request = id,
                                      .segment = std::move(*segment)});
    send_to_engine(ipc::Message{.request_id = request, .body = ipc::msg::Attach{}}, fds);
}

bool Supervisor::ring_attached(std::uint32_t id, std::uint32_t slot) {
    const auto found = std::ranges::find(attaches_, id, &PendingAttach::request);
    if (found == attaches_.end()) {
        return false;
    }
    PendingAttach pending = std::move(*found);
    attaches_.erase(found);
    Client* const client = find_client(pending.client);
    if (client == nullptr) {
        // Its client left while the ring was being opened.
        detach_ring(slot, std::move(pending.segment));
        return true;
    }
    client->opening = false;
    info(std::format("client '{}' opened a ring in slot {}", client->name, slot));
    const std::array<int, 1> fds{pending.segment.get()};
    // The ring is the client's before the answer is sent: if sending drops
    // the client, drop() detaches it.
    client->ring =
        std::make_unique<Ring>(Ring{.slot = slot, .segment = std::move(pending.segment)});
    send_to_client(*client,
                   ipc::Message{.request_id = pending.client_request,
                                .body = ipc::msg::RingOpened{.slot = slot}},
                   fds);
    return true;
}

bool Supervisor::ring_detached(std::uint32_t id) {
    const auto found = std::ranges::find(detaches_, id, &PendingDetach::request);
    if (found == detaches_.end()) {
        return false;
    }
    info(std::format("the ring in slot {} was detached", found->slot));
    detaches_.erase(found);
    return true;
}

bool Supervisor::attach_refused(std::uint32_t id, const ipc::msg::Error& error) {
    const auto found = std::ranges::find(attaches_, id, &PendingAttach::request);
    if (found == attaches_.end()) {
        return false;
    }
    const PendingAttach pending = std::move(*found);
    attaches_.erase(found);
    Client* const client = find_client(pending.client);
    if (client == nullptr) {
        warning(std::format("ks-engine refused a ring: {}", error.text));
        return true;
    }
    client->opening = false;
    warning(std::format("refused client '{}' a ring: {}", client->name, error.text));
    send_to_client(*client,
                   error_message(pending.client_request, ipc::ErrorCode::Unavailable, error.text));
    return true;
}

bool Supervisor::detach_refused(std::uint32_t id, const ipc::msg::Error& error) {
    const auto found = std::ranges::find(detaches_, id, &PendingDetach::request);
    if (found == detaches_.end()) {
        return false;
    }
    warning(std::format("ks-engine refused to detach the ring in slot {}: {}", found->slot,
                        error.text));
    detaches_.erase(found);
    return true;
}

void Supervisor::detach_ring(std::uint32_t slot, ipc::Fd segment) {
    const std::uint32_t request = next_request();
    detaches_.push_back(
        PendingDetach{.request = request, .slot = slot, .segment = std::move(segment)});
    send_to_engine(ipc::Message{.request_id = request, .body = ipc::msg::Detach{.slot = slot}});
}

void Supervisor::engine_failed(std::string_view why) {
    engine_.reset();
    if (phase_ == Phase::Stopping || phase_ == Phase::Terminating) {
        // It's on its way out: wait for it.
        return;
    }
    error(std::format("ks-engine's connection {}; stopping it", why));
    if (phase_ == Phase::Starting) {
        forced_status_ = kExitStartFailed;
    }
    ::kill(engine_pid_, SIGTERM);
    phase_ = Phase::Terminating;
    deadline_ = Clock::now() + kEngineStopTimeout;
}

void Supervisor::accept_clients() {
    for (;;) {
        std::expected<std::optional<ipc::Fd>, std::error_code> accepted =
            listener_->accept(::geteuid());
        if (!accepted) {
            if (accepted.error() == std::errc::permission_denied) {
                warning("refused a connection from another user");
                continue;
            }
            warning(std::format("can't accept a connection: {}", accepted.error().message()));
            return;
        }
        std::optional<ipc::Fd>& socket = *accepted;
        if (!socket) {
            return;
        }
        if (clients_.size() >= kMaxConnections) {
            warning(std::format("refused a connection: {} are open already", kMaxConnections));
            continue;
        }
        clients_.push_back(Client{.connection = ipc::Connection{std::move(*socket)},
                                  .id = next_client_++,
                                  .welcomed = false,
                                  .name = {},
                                  .stops = {},
                                  .ring = nullptr,
                                  .opening = false,
                                  .dropped = false});
    }
}

void Supervisor::handle_client(Client& client, short events) {
    if ((events & POLLOUT) != 0 && !client.connection.flush()) {
        drop(client, "couldn't be written to");
        return;
    }
    while (!client.dropped) {
        std::expected<std::optional<ipc::ReceivedFrame>, ipc::ConnectionFailure> received =
            client.connection.receive();
        if (!received) {
            switch (received.error().error) {
            case ipc::ConnectionError::BadFrame:
                refuse_and_drop(client, 0, "a bad frame");
                return;
            case ipc::ConnectionError::Closed:
                drop(client, "closed its connection", ipc::LogLevel::Info);
                return;
            default:
                drop(client, "closed its connection mid-message");
                return;
            }
        }
        const std::optional<ipc::ReceivedFrame>& frame = *received;
        if (!frame) {
            return;
        }
        const std::expected<ipc::Message, ipc::MessageError> message = ipc::decode(frame->frame);
        if (!message) {
            refuse_and_drop(client, frame->frame.request_id, "a message that doesn't decode");
            return;
        }
        handle_client_message(client, *message);
    }
}

void Supervisor::handle_client_message(Client& client, const ipc::Message& message) {
    const std::uint32_t id = message.request_id;
    if (const auto* const hello = std::get_if<ipc::msg::Hello>(&message.body)) {
        if (client.welcomed || phase_ != Phase::Running) {
            send_to_client(client,
                           error_message(id, ipc::ErrorCode::NotSupported,
                                         client.welcomed ? "Hello was already said" : "stopping"));
            return;
        }
        client.welcomed = true;
        client.name = hello->name;
        info(std::format("client '{}' said Hello", client.name));
        const std::array<int, 1> state{state_.get()};
        send_to_client(client, ipc::Message{.request_id = id, .body = ipc::msg::Welcome{}}, state);
        return;
    }
    if (!client.welcomed) {
        refuse_and_drop(client, id, "the first message must be Hello");
        return;
    }
    if (std::holds_alternative<ipc::msg::Ping>(message.body)) {
        send_to_client(client, ipc::Message{.request_id = id, .body = ipc::msg::Pong{}});
    } else if (std::holds_alternative<ipc::msg::Stop>(message.body)) {
        client.stops.push_back(id);
        begin_stop(std::format("client '{}' asked", client.name));
    } else if (std::holds_alternative<ipc::msg::OpenRing>(message.body)) {
        open_ring(client, id);
    } else if (const auto* const error = std::get_if<ipc::msg::Error>(&message.body)) {
        warning(std::format("client '{}' reported an error: {}", client.name, error->text));
    } else {
        send_to_client(client,
                       error_message(id, ipc::ErrorCode::NotSupported, "not from a client"));
    }
}

void Supervisor::send_to_client(Client& client, const ipc::Message& message,
                                std::span<const int> fds) {
    if (client.dropped) {
        return;
    }
    const std::expected<ipc::Frame, ipc::MessageError> frame = ipc::encode(message);
    if (!frame) {
        return;
    }
    if (!client.connection.send(*frame, fds)) {
        drop(client, "isn't reading its messages");
        return;
    }
    if (!client.connection.flush()) {
        drop(client, "couldn't be written to");
    }
}

void Supervisor::refuse_and_drop(Client& client, std::uint32_t request_id, std::string why) {
    const std::string reason = std::format("sent {}", why);
    send_to_client(client, error_message(request_id, ipc::ErrorCode::BadMessage, std::move(why)));
    drop(client, reason);
}

void Supervisor::drop(Client& client, std::string_view why, ipc::LogLevel level) {
    if (client.dropped) {
        return;
    }
    client.dropped = true;
    report(level, client.name.empty()
                      ? std::format("dropped a client: it {}", why)
                      : std::format("dropped client '{}': it {}", client.name, why));
    // A ring being opened is detached when the engine's answer comes
    // (ring_attached()).
    if (client.ring) {
        detach_ring(client.ring->slot, std::move(client.ring->segment));
        client.ring.reset();
    }
}

void Supervisor::begin_stop(std::string_view why) {
    if (phase_ == Phase::Stopping || phase_ == Phase::Terminating) {
        return;
    }
    info(std::format("stopping: {}", why));
    listener_.reset();
    phase_ = Phase::Stopping;
    deadline_ = Clock::now() + kEngineStopTimeout;
    send_to_engine(ipc::Message{.request_id = next_request(), .body = ipc::msg::Stop{}});
}

std::optional<int> Supervisor::deadline_passed() {
    if (!deadline_ || Clock::now() < *deadline_) {
        return std::nullopt;
    }
    switch (phase_) {
    case Phase::Starting:
        error(std::format("ks-engine didn't report ready within {} s; killing it",
                          kEngineReadyTimeout.count()));
        kill_engine();
        return kExitStartFailed;
    case Phase::Stopping:
    case Phase::Terminating:
        error(std::format("ks-engine didn't stop within {} s; killing it",
                          kEngineStopTimeout.count()));
        kill_engine();
        return forced_status_.value_or(kExitEngineFailed);
    case Phase::Running:
        break;
    }
    deadline_.reset();
    return std::nullopt;
}

void Supervisor::kill_engine() {
    if (engine_pid_ <= 0) {
        return;
    }
    ::kill(engine_pid_, SIGKILL);
    int status = 0;
    while (::waitpid(engine_pid_, &status, 0) == -1 && errno == EINTR) {
    }
    engine_pid_ = 0;
    drain_engine_log();
}

void Supervisor::flush_clients() {
    const Clock::time_point deadline = Clock::now() + kClientFlushTimeout;
    for (;;) {
        std::vector<pollfd> polled;
        for (const Client& client : clients_) {
            if (!client.dropped && client.connection.has_queued()) {
                polled.push_back(
                    {.fd = client.connection.socket(), .events = POLLOUT, .revents = 0});
            }
        }
        if (polled.empty() || Clock::now() >= deadline) {
            return;
        }
        static_cast<void>(
            ::poll(polled.data(), static_cast<nfds_t>(polled.size()), poll_timeout(deadline)));
        for (Client& client : clients_) {
            if (!client.dropped && client.connection.has_queued() && !client.connection.flush()) {
                client.dropped = true;
            }
        }
    }
}

} // namespace

std::expected<std::filesystem::path, std::error_code> default_engine_path() {
    std::error_code error;
#ifdef __APPLE__
    std::uint32_t size = 0;
    static_cast<void>(::_NSGetExecutablePath(nullptr, &size));
    std::string path(size, '\0');
    if (::_NSGetExecutablePath(path.data(), &size) != 0) {
        return std::unexpected{std::make_error_code(std::errc::no_such_file_or_directory)};
    }
    path.resize(path.find('\0'));
    const std::filesystem::path self = std::filesystem::canonical(path, error);
#else
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
#endif
    if (error) {
        return std::unexpected{error};
    }
    return self.parent_path() / "ks-engine";
}

int run(const Options& options, std::ostream& out, std::ostream& err) {
    if (options.help) {
        out << usage();
        return kExitOk;
    }
    Supervisor supervisor{options, err};
    return supervisor.run();
}

} // namespace ks::supervisor
