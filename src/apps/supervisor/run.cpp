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
#include <ks/ipc/recording.h>
#include <ks/ipc/region.h>
#include <ks/ipc/runtime_dir.h>
#include <ks/ipc/segment.h>
#include <ks/ipc/state.h>
#include <ks/ipc/track_segment.h>
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
#include <limits>
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

/// `text`, cut to at most `size` bytes at the start of a UTF-8 character.
std::string clipped(std::string text, std::size_t size) {
    if (text.size() <= size) {
        return text;
    }
    std::size_t end = size;
    // Back up over continuation bytes, to the start of the character cut.
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
        --end;
    }
    text.resize(end);
    return text;
}

/// An Error message, with its text cut to the most an Error carries.
ipc::Message error_message(std::uint32_t request_id, ipc::ErrorCode code, std::string text) {
    return ipc::Message{
        .request_id = request_id,
        .body = ipc::msg::Error{.code = code,
                                .text = clipped(std::move(text), ipc::kMaxErrorTextSize)}};
}

/// Bytes in a MiB.
constexpr std::uint64_t kMib = std::uint64_t{1'024} * 1'024;

/// `bytes` in MiB, rounded up.
std::uint64_t mib_up(std::uint64_t bytes) {
    return (bytes + kMib - 1) / kMib;
}

/// Opens `path` for reading as a track's file (specs/track-loading/spec.md
/// requirement 11): with O_NONBLOCK, so that a FIFO can't block the poll
/// loop, and only if it's a regular file. Says why not, without the path:
/// the client knows it.
std::expected<ipc::Fd, std::string> open_track_file(const std::string& path) {
    int fd = -1;
    for (;;) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open() is variadic
        fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
        if (fd != -1 || errno != EINTR) {
            break;
        }
    }
    if (fd == -1) {
        return std::unexpected{std::format(
            "can't open the file: {}", std::error_code{errno, std::system_category()}.message())};
    }
    ipc::Fd file{fd};
    struct stat status{};
    if (::fstat(fd, &status) != 0) {
        return std::unexpected{
            std::format("can't read the file's status: {}",
                        std::error_code{errno, std::system_category()}.message())};
    }
    if (!S_ISREG(status.st_mode)) {
        return std::unexpected{std::string{"it isn't a regular file"}};
    }
    return file;
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
    std::uint32_t slot = 0;           ///< The slot the Attach asked for.
    /// The ring is attached again to a restarted engine: the client has it
    /// already, and the segment stays in Client::ring (specs/engine-restart/
    /// spec.md requirement 2).
    bool reattach = false;
    ipc::Fd segment;
};

/// A Detach sent to the engine, waiting for its answer. The supervisor keeps
/// its descriptor of the segment until then (requirement 11).
struct PendingDetach {
    std::uint32_t request = 0; ///< The Detach's request ID.
    std::uint32_t slot = 0;
    ipc::Fd segment;
};

/// An UnloadDeck sent to the engine, waiting for its answer
/// (specs/ks-play/spec.md requirement 5).
struct PendingUnload {
    std::uint32_t request = 0;        ///< The UnloadDeck's request ID.
    std::uint64_t client = 0;         ///< Client::id.
    std::uint32_t client_request = 0; ///< The client's UnloadDeck's request ID.
    std::uint32_t deck = 0;
};

/// Where the engine has got with a load's track (specs/track-loading/spec.md
/// requirements 12 and 17).
enum class EngineHold : std::uint8_t {
    None,    ///< It hasn't been sent the track.
    Waiting, ///< A LoadTrack waits for its answer.
    Holds,   ///< It has the track.
    Done,    ///< It refused the track, or released it.
};

/// The request IDs the supervisor gives a worker's Probe and Decode.
constexpr std::uint32_t kProbeRequest = 1;
constexpr std::uint32_t kDecodeRequest = 2;

/// One LoadFile, from its worker starting until the supervisor closes its
/// track's segment (specs/track-loading/spec.md requirements 11 to 17).
struct Load {
    std::uint64_t track_id = 0;
    /// The deck's index, from 0.
    std::uint32_t deck = 0;
    std::string path;
    /// The client that asked (Client::id), its name for reports, and its
    /// LoadFile's request ID.
    std::uint64_t client = 0;
    std::string client_name;
    std::uint32_t client_request = 0;
    /// The client has had its answer, or been refused.
    bool answered = false;
    /// The client has had its FileLoaded.
    bool file_loaded = false;
    /// How decoding ended, once it has, and whether the client has been
    /// told (specs/ks-play/spec.md requirement 8).
    std::optional<ipc::msg::DecodeEnded> ended;
    bool notified = false;

    /// The worker's process ID until it's reaped, then 0.
    pid_t worker_pid = 0;
    /// The worker's connection; null once closed.
    std::unique_ptr<ipc::Connection> worker;
    /// The worker's log segment, the supervisor's mapping of it, and the
    /// reader of its ring, until the worker has been reaped.
    ipc::Fd log_segment;
    std::unique_ptr<ipc::Region> log_region;
    std::unique_ptr<ipc::LogReader> log_reader;
    /// What the worker probed, once it has.
    std::optional<ipc::msg::Probed> probed;
    bool decode_sent = false;
    /// Decoding has ended: Decoded came, or the track's header is complete.
    bool decode_ended = false;

    /// The track segment, and the supervisor's read-write mapping of it,
    /// once created.
    ipc::Fd segment;
    std::unique_ptr<ipc::Region> mapping;
    /// What the track counts against the budget, in bytes (requirement 16).
    std::uint64_t reserved = 0;
    EngineHold engine = EngineHold::None;
    /// The LoadTrack's request ID, while the engine's answer is awaited.
    std::uint32_t engine_request = 0;

    /// True once nothing more can happen to it: its worker has been reaped,
    /// and the engine doesn't hold its track or owe an answer about it.
    [[nodiscard]] bool over() const noexcept {
        return worker_pid == 0 && engine != EngineHold::Waiting && engine != EngineHold::Holds;
    }
};

class Supervisor {
public:
    Supervisor(const Options& options, std::ostream& err) : options_{&options}, err_{&err} {}

    int run() {
        int status = start();
        while (status == kExitOk) {
            if (const std::optional<int> done = step()) {
                status = *done;
                break;
            }
        }
        // No worker outlives the supervisor (specs/track-loading/spec.md
        // requirement 18). After a clean stop, none is left by now.
        stop_workers(Clock::now() + kWorkerStopTimeout);
        return status;
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
    /// Starts ks-engine as at launch, or `clean` as requirement 11 of
    /// specs/engine-restart/spec.md says: sets engine_pid_, engine_, and the
    /// Ready deadline. Says why not.
    std::expected<void, std::string> spawn_engine(bool clean);
    /// The engine process has ended, or been killed, while a session was
    /// running: answers what waited for it, counts the exit, and starts the
    /// next engine, or gives up (requirements 2, 8, and 11). Returns the exit
    /// status if the supervisor must exit.
    std::optional<int> engine_gone();
    /// Answers every request still waiting for the engine that has gone
    /// (requirement 8).
    void answer_waiting_requests();
    /// Zero-fills the engine-state segment past its header (requirement 11).
    void wipe_state();
    /// Sends an Attach for every client's ring, to the engine that has just
    /// said Ready (requirement 2).
    void reattach_rings();
    /// Ends the restart once no reattach waits for an answer.
    void settle_restart();
    /// The lowest client slot nothing holds, if one is free.
    [[nodiscard]] std::optional<std::uint32_t> free_slot() const;
    /// Handles a client's OpenRecording (requirement 17).
    void open_recording(Client& client, std::uint32_t id);
    [[nodiscard]] std::chrono::milliseconds ready_timeout() const {
        return options_->ready_timeout_ms ? std::chrono::milliseconds{*options_->ready_timeout_ms}
                                          : std::chrono::milliseconds{kEngineReadyTimeout};
    }
    [[nodiscard]] std::chrono::milliseconds crash_window() const {
        return options_->crash_window_ms ? std::chrono::milliseconds{*options_->crash_window_ms}
                                         : std::chrono::milliseconds{kCrashLoopWindow};
    }
    [[nodiscard]] std::chrono::milliseconds hang_timeout() const {
        return options_->hang_timeout_ms ? std::chrono::milliseconds{*options_->hang_timeout_ms}
                                         : kHangTimeout;
    }
    /// A fifth of the hang timeout, at least 10 ms (requirement 14).
    [[nodiscard]] std::chrono::milliseconds heartbeat_poll() const {
        return options_->hang_timeout_ms
                   ? std::max(std::chrono::milliseconds{10}, hang_timeout() / 5)
                   : kHeartbeatPoll;
    }
    [[nodiscard]] std::chrono::milliseconds ping_interval() const {
        return options_->ping_interval_ms ? std::chrono::milliseconds{*options_->ping_interval_ms}
                                          : kEnginePingInterval;
    }
    /// Watching the engine for hangs (specs/hang-detection/spec.md): starts
    /// at its Ready, and stops when it ends.
    void start_watch();
    void stop_watch() noexcept;
    /// One pass of the watch: pings, the heartbeat, and a kill if either
    /// calls for one.
    void watch_engine();
    /// The engine's status, and whether any deck plays, from the state
    /// segment. Nothing / true if they can't be read.
    [[nodiscard]] std::optional<ipc::EngineStatus> read_status();
    [[nodiscard]] bool any_deck_playing() const;
    /// Handles a Pong with request ID `id`.
    void pong(std::uint32_t id);
    /// Reports `why` and sends the engine SIGKILL. The kill is reaped, and
    /// the engine restarted, as for any exit.
    void kill_hung_engine(std::string_view why);
    /// True once begin_stop() has run.
    [[nodiscard]] bool stopping() const {
        return phase_ == Phase::Stopping || phase_ == Phase::Terminating;
    }
    /// Answers `id` with Unavailable "ks-engine is restarting" and returns
    /// true if a restart is under way (requirement 9).
    bool refuse_while_restarting(Client& client, std::uint32_t id);

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

    /// How long the poll loop may wait, in milliseconds; -1 for no limit.
    [[nodiscard]] int poll_wait() const;
    /// One pass of the poll loop. Returns the exit status once the
    /// supervisor is done.
    std::optional<int> step();

    std::optional<int> handle_signals();
    std::optional<int> engine_exited(int status);
    void handle_engine(short events);
    void handle_engine_message(const ipc::Message& message);
    /// Handles the engine's TrackLoaded, DeckUnloaded, and TrackReleased.
    /// Returns false for any other message.
    bool handle_engine_track_message(const ipc::Message& message);
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

    // ---- Loading tracks (specs/track-loading/spec.md) ----

    /// Works out the worker's path and the budget, and checks the worker
    /// (requirement 10). Returns kExitOk, or reports and returns
    /// kExitStartFailed.
    int prepare_loading();
    /// Handles a client's LoadFile (requirement 11).
    void load_file(Client& client, std::uint32_t id, const ipc::msg::LoadFile& request);
    /// Creates the worker's log segment and socket, and starts ks-worker for
    /// `load`. Says why not.
    std::expected<void, std::string> start_worker(Load& load);
    /// Answers `load`'s client with an Error, unless it has been answered,
    /// and reports the refusal.
    void refuse_load(Load& load, ipc::ErrorCode code, const std::string& why);
    /// Handles what a worker sent, as handle_client() does a client's.
    void handle_worker(Load& load, short events);
    void handle_worker_message(Load& load, const ipc::Message& message);
    /// Handles a worker's Probed (requirement 12).
    void probed(Load& load, const ipc::msg::Probed& probed);
    /// Handles a worker's Decoded.
    void decoded(Load& load, const ipc::msg::Decoded& decoded);
    /// Records how `load`'s decoding ended (specs/ks-play/spec.md
    /// requirement 8) and tells its client if it can be told.
    void decoding_ended(Load& load, std::uint64_t frames, ipc::TrackStatus status);
    /// Sends `load`'s client its DecodeEnded, once it has had FileLoaded,
    /// and decoding has ended, and not before or twice.
    void notify_decode_ended(Load& load);
    /// Handles a client's UnloadDeck (specs/ks-play/spec.md requirement 5),
    /// and the engine's answers to the UnloadDecks it sent.
    void unload_deck(Client& client, std::uint32_t id, const ipc::msg::UnloadDeck& request);
    bool deck_unloaded(std::uint32_t id);
    bool unload_refused(std::uint32_t id, const ipc::msg::Error& error);
    void send_to_worker(Load& load, const ipc::Message& message, std::span<const int> fds = {});
    /// Closes a worker's connection, saying why at `level` if `why` isn't
    /// empty, and sends it SIGTERM if `stop`.
    void close_worker(Load& load, std::string_view why = {}, bool stop = false);
    /// Sends `load`'s worker `signal`, if it's running.
    static void signal_worker(const Load& load, int signal);
    /// The engine's answers about tracks. Each returns false if it answers
    /// nothing the supervisor sent, or names no track it holds.
    bool track_loaded(std::uint32_t id);
    bool load_track_refused(std::uint32_t id, const ipc::msg::Error& error);
    bool track_released(std::uint64_t track_id);
    /// Reaps every worker that has ended (requirement 15).
    void reap_workers();
    void worker_ended(Load& load, int status);
    /// Finishes a track whose worker ended partway (requirement 15).
    void finish_track(Load& load, std::string_view how);
    /// Reads every running worker's log ring, as the engine's.
    void drain_worker_logs();
    void drain_worker_log(Load& load);
    /// One of a worker's lines, in ks-worker.log or on standard error
    /// (requirement 19).
    void worker_line(ipc::LogLevel level, std::uint64_t time, std::string_view text);
    /// Creates ks-worker.log, the first time a worker starts, if there are
    /// log files.
    void open_worker_log();
    /// Stops every running worker: SIGTERM, then SIGKILL for any still
    /// running at `deadline`. Reaps them all (requirement 18).
    void stop_workers(Clock::time_point deadline);
    [[nodiscard]] std::size_t running_workers() const;
    /// The bytes counted against the budget.
    [[nodiscard]] std::uint64_t in_use() const;

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
    std::filesystem::path engine_path_;
    /// The recording segment, with --recording.
    ipc::Fd recording_;
    /// The engine has said Ready once: there's a session to carry on.
    bool had_ready_ = false;
    /// A replacement engine is starting, or its rings are being attached
    /// (requirement 9).
    bool restarting_ = false;
    /// When the engines that ended in the crash-loop window ended.
    std::vector<Clock::time_point> exits_;
    /// How many times an engine has been restarted, and when the last one
    /// was reaped.
    std::uint64_t restarts_ = 0;
    Clock::time_point reaped_;
    std::uint32_t next_request_ = 1;

    /// Hang detection (specs/hang-detection/spec.md): the supervisor's
    /// read-only mapping of the state segment, and the watch's state.
    struct Watch {
        std::unique_ptr<ipc::Region> state_view;
        std::uint64_t last_heartbeat = 0;
        Clock::time_point heartbeat_changed;
        Clock::time_point next_poll;
        Clock::time_point next_ping;
        /// When the ping that hasn't been answered was sent.
        Clock::time_point ping_sent;
        /// Its request ID; 0 if none is waiting.
        std::uint32_t ping_id = 0;
        bool watching = false;
        /// The engine's heartbeat has been seen to be non-zero: only then can
        /// it stand still (requirement 3).
        bool beating = false;
        bool hang_killed = false;
        bool unresponsive = false;
    };
    Watch watch_;

    std::unique_ptr<ipc::Listener> listener_;
    std::vector<Client> clients_;
    std::uint64_t next_client_ = 1;
    std::vector<PendingAttach> attaches_;
    std::vector<PendingDetach> detaches_;
    std::vector<PendingUnload> unloads_;

    /// The worker program, the track memory budget in bytes, and every load
    /// not yet over.
    std::filesystem::path worker_path_;
    std::uint64_t budget_ = 0;
    std::vector<std::unique_ptr<Load>> loads_;
    /// The next track ID: from 1, never reused (requirement 11).
    std::uint64_t next_track_ = 1;
    /// Each deck's latest load: the track ID of the last LoadFile a worker
    /// was started for (requirement 13).
    std::array<std::uint64_t, ipc::kStateDecks> latest_{};
    /// What latest_ holds for a deck whose latest request was an unload:
    /// never a track ID (IDs count up from 1).
    static constexpr std::uint64_t kUnloaded = std::numeric_limits<std::uint64_t>::max();
    /// ks-worker.log; null until the first worker starts, or if it couldn't
    /// be created.
    std::unique_ptr<LogFile> worker_log_;
    bool worker_log_tried_ = false;

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
    if (const int prepared = prepare_loading(); prepared != kExitOk) {
        return prepared;
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

    engine_path_ = std::move(engine);
    if (options_->recording) {
        std::expected<ipc::Fd, std::error_code> recording = factory_->create(
            ipc::SegmentKind::Recording, ipc::kRecordingLayoutVersion, ipc::kRecordingSegmentSize);
        if (!recording) {
            error(
                std::format("can't create the recording segment: {}", recording.error().message()));
            return kExitStartFailed;
        }
        recording_ = std::move(*recording);
    }
    open_engine_log();
    if (const std::expected<void, std::string> started = spawn_engine(false); !started) {
        error(started.error());
        return kExitStartFailed;
    }
    return kExitOk;
}

std::expected<void, std::string> Supervisor::spawn_engine(bool clean) {
    std::expected<std::pair<ipc::Fd, ipc::Fd>, std::error_code> pair = ipc::socket_pair();
    if (!pair || !set_nonblocking_cloexec(pair->first.get())) {
        return std::unexpected{std::string{"can't create the engine's socket"}};
    }
    std::vector<std::string> args{"--supervisor-fd", "3", "--state-fd",    "4",
                                  "--log-fd",        "5", "--runtime-dir", dir_.string()};
    std::vector<Inherit> fds{Inherit{.parent = pair->second.get(), .child = 3},
                             Inherit{.parent = state_.get(), .child = 4},
                             Inherit{.parent = engine_log_segment_.get(), .child = 5}};
    if (recording_.valid()) {
        args.insert(args.end(), {"--record-fd", "6"});
        fds.push_back(Inherit{.parent = recording_.get(), .child = 6});
    }
    if (clean) {
        args.emplace_back("--clean");
    } else {
        // The tracks the engine holds, in the order of their IDs, from
        // descriptor 7 (requirement 4).
        std::vector<const Load*> held;
        for (const std::unique_ptr<Load>& load : loads_) {
            if (load->segment.valid() &&
                (load->engine == EngineHold::Holds || load->engine == EngineHold::Waiting)) {
                held.push_back(load.get());
            }
        }
        std::ranges::sort(held, {}, &Load::track_id);
        if (held.size() > kRestartTracks) {
            held.resize(kRestartTracks);
        }
        int next = 7;
        for (const Load* const load : held) {
            args.insert(args.end(), {"--track", std::format("{}:{}", load->track_id, next)});
            fds.push_back(Inherit{.parent = load->segment.get(), .child = next});
            ++next;
        }
    }
    args.insert(args.end(), options_->engine_args.begin(), options_->engine_args.end());
    const std::expected<pid_t, std::error_code> pid = spawn(engine_path_, args, fds);
    if (!pid) {
        return std::unexpected{
            std::format("can't start {}: {}", engine_path_.string(), pid.error().message())};
    }
    stop_watch();
    engine_pid_ = *pid;
    engine_ = std::make_unique<ipc::Connection>(std::move(pair->first));
    info(std::format("started ks-engine (process {})", engine_pid_));
    deadline_ = Clock::now() + ready_timeout();
    return {};
}

std::optional<int> Supervisor::engine_gone() {
    engine_pid_ = 0;
    engine_.reset();
    reaped_ = Clock::now();
    answer_waiting_requests();
    const std::chrono::milliseconds window = crash_window();
    for (;;) {
        const Clock::time_point now = Clock::now();
        exits_.push_back(now);
        std::erase_if(exits_, [&](Clock::time_point at) { return now - at > window; });
        if (exits_.size() > kCleanRestartExit) {
            error("ks-engine keeps failing; not restarting it");
            listener_.reset();
            clients_.clear();
            return kExitEngineFailed;
        }
        const bool clean = exits_.size() == kCleanRestartExit;
        ++restarts_;
        info(std::format("restarting ks-engine (restart {})", restarts_));
        if (clean) {
            info("starting it clean: its decks come back empty and paused");
            wipe_state();
            // The new engine holds no track.
            for (const std::unique_ptr<Load>& load : loads_) {
                if (load->engine == EngineHold::Holds) {
                    load->engine = EngineHold::Done;
                    signal_worker(*load, SIGTERM);
                }
            }
        }
        restarting_ = true;
        phase_ = Phase::Starting;
        const std::expected<void, std::string> started = spawn_engine(clean);
        if (started) {
            return std::nullopt;
        }
        error(started.error());
    }
}

void Supervisor::answer_waiting_requests() {
    const std::string why = "ks-engine restarted; ask again";
    for (const std::unique_ptr<Load>& load : loads_) {
        if (load->engine == EngineHold::Waiting) {
            load->engine = EngineHold::Done;
            refuse_load(*load, ipc::ErrorCode::Unavailable, why);
            signal_worker(*load, SIGTERM);
        }
    }
    for (const PendingAttach& pending : std::exchange(attaches_, {})) {
        if (pending.reattach) {
            continue;
        }
        if (Client* const client = find_client(pending.client)) {
            client->opening = false;
            warning(std::format("refused client '{}' a ring: {}", client->name, why));
            send_to_client(*client,
                           error_message(pending.client_request, ipc::ErrorCode::Unavailable, why));
        }
    }
    for (const PendingUnload& pending : std::exchange(unloads_, {})) {
        if (Client* const client = find_client(pending.client)) {
            send_to_client(*client,
                           error_message(pending.client_request, ipc::ErrorCode::Unavailable, why));
        }
    }
    // The rings of the Detaches that waited are gone with the engine.
    detaches_.clear();
}

void Supervisor::wipe_state() {
    std::expected<ipc::Region, ipc::MapFailure> region =
        ipc::Region::map(state_.get(), ipc::SegmentKind::EngineState,
                         ipc::kEngineStateLayoutVersion, ipc::Access::ReadWrite);
    if (!region) {
        warning("can't map the engine-state segment to clear it");
        return;
    }
    const std::span<std::byte> bytes = region->bytes();
    if (bytes.size() > ipc::kSegmentHeaderSize) {
        std::ranges::fill(bytes.subspan(ipc::kSegmentHeaderSize), std::byte{0});
    }
}

void Supervisor::reattach_rings() {
    for (Client& client : clients_) {
        if (client.dropped || !client.ring) {
            continue;
        }
        const std::uint32_t request = next_request();
        attaches_.push_back(PendingAttach{.request = request,
                                          .client = client.id,
                                          .client_request = 0,
                                          .slot = client.ring->slot,
                                          .reattach = true,
                                          .segment = {}});
        const std::array<int, 1> fds{client.ring->segment.get()};
        send_to_engine(ipc::Message{.request_id = request,
                                    .body = ipc::msg::Attach{.slot = client.ring->slot}},
                       fds);
    }
    settle_restart();
}

void Supervisor::settle_restart() {
    if (restarting_ && phase_ == Phase::Running &&
        std::ranges::none_of(attaches_, &PendingAttach::reattach)) {
        restarting_ = false;
    }
}

std::optional<std::uint32_t> Supervisor::free_slot() const {
    std::array<bool, ipc::kStateClients> used{};
    const auto take = [&used](std::uint32_t slot) {
        if (slot < used.size()) {
            std::span{used}[slot] = true;
        }
    };
    for (const Client& client : clients_) {
        if (client.ring) {
            take(client.ring->slot);
        }
    }
    for (const PendingAttach& pending : attaches_) {
        take(pending.slot);
    }
    // A slot whose Detach is still waiting is free to ask for: the engine
    // refuses the Attach until it has let the old ring go, and the client
    // asks again.
    for (std::uint32_t slot = 0; slot < used.size(); ++slot) {
        if (!std::span{used}[slot]) {
            return slot;
        }
    }
    return std::nullopt;
}

void Supervisor::open_recording(Client& client, std::uint32_t id) {
    if (!recording_.valid()) {
        send_to_client(client, error_message(id, ipc::ErrorCode::NotSupported,
                                             "kool_spin wasn't started with --recording"));
        return;
    }
    const std::array<int, 1> fds{recording_.get()};
    send_to_client(client, ipc::Message{.request_id = id, .body = ipc::msg::RecordingOpened{}},
                   fds);
}

bool Supervisor::refuse_while_restarting(Client& client, std::uint32_t id) {
    if (stopping()) {
        return false;
    }
    if (!restarting_) {
        if (!watch_.unresponsive) {
            return false;
        }
        // The engine is running but its control thread isn't answering
        // (specs/hang-detection/spec.md requirement 13).
        send_to_client(
            client, error_message(id, ipc::ErrorCode::Unavailable, "ks-engine is not answering"));
        return true;
    }
    send_to_client(client,
                   error_message(id, ipc::ErrorCode::Unavailable, "ks-engine is restarting"));
    return true;
}

// ---- Hang detection (specs/hang-detection/spec.md) ----------------------------

void Supervisor::start_watch() {
    const Clock::time_point now = Clock::now();
    watch_.watching = true;
    watch_.hang_killed = false;
    watch_.unresponsive = false;
    watch_.ping_id = 0;
    watch_.heartbeat_changed = now;
    watch_.next_poll = now + heartbeat_poll();
    // The first ping is one interval after Ready (requirement 12).
    watch_.next_ping = now + ping_interval();
    watch_.beating = false;
    watch_.last_heartbeat = 0;
}

void Supervisor::stop_watch() noexcept {
    watch_.watching = false;
    watch_.beating = false;
    watch_.hang_killed = false;
    watch_.unresponsive = false;
    watch_.ping_id = 0;
}

std::optional<ipc::EngineStatus> Supervisor::read_status() {
    // Every call into a seqlock runs inside this (see SignalsBlocked).
    const SignalsBlocked blocked;
    if (!watch_.state_view) {
        std::expected<ipc::Region, ipc::MapFailure> region =
            ipc::Region::map(state_.get(), ipc::SegmentKind::EngineState,
                             ipc::kEngineStateLayoutVersion, ipc::Access::ReadOnly);
        if (!region) {
            return std::nullopt;
        }
        watch_.state_view = std::make_unique<ipc::Region>(std::move(*region));
    }
    // at() checks against the size of the supervisor's own mapping.
    auto* const state = watch_.state_view->at<ipc::EngineState>(ipc::kSegmentHeaderSize);
    if (state == nullptr) {
        return std::nullopt;
    }
    return ipc::SeqlockReader<ipc::EngineStatus>{state->status}.try_read(1'000);
}

bool Supervisor::any_deck_playing() const {
    const SignalsBlocked blocked;
    auto* const state = watch_.state_view
                            ? watch_.state_view->at<ipc::EngineState>(ipc::kSegmentHeaderSize)
                            : nullptr;
    if (state == nullptr) {
        return true;
    }
    for (auto& slot : state->decks) {
        const std::optional<ipc::DeckState> deck =
            ipc::SeqlockReader<ipc::DeckState>{slot}.try_read(1'000);
        // A deck that can't be read counts as playing (requirement 11). One
        // that has published nothing isn't.
        if (!deck || (deck->heartbeat != 0 && (deck->flags & ipc::kDeckPlaying) != 0)) {
            return true;
        }
    }
    return false;
}

void Supervisor::pong(std::uint32_t id) {
    if (id == 0 || id != watch_.ping_id) {
        return;
    }
    watch_.ping_id = 0;
    if (watch_.unresponsive) {
        watch_.unresponsive = false;
        info("ks-engine control thread answering again");
    }
}

void Supervisor::kill_hung_engine(std::string_view why) {
    watch_.hang_killed = true;
    warning(std::format("ks-engine {}; killing it", why));
    if (engine_pid_ > 0) {
        ::kill(engine_pid_, SIGKILL);
    }
}

void Supervisor::watch_engine() {
    if (!watch_.watching || watch_.hang_killed || phase_ != Phase::Running || engine_pid_ <= 0 ||
        !engine_ || stopping()) {
        return;
    }
    const Clock::time_point now = Clock::now();
    const auto ms = [](Clock::duration d) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    };

    // Pings: one at a time, one interval apart.
    if (watch_.ping_id == 0 && now >= watch_.next_ping) {
        watch_.ping_id = next_request();
        watch_.ping_sent = now;
        watch_.next_ping = now + ping_interval();
        send_to_engine(ipc::Message{.request_id = watch_.ping_id, .body = ipc::msg::Ping{}});
    }
    if (watch_.ping_id != 0 && !watch_.unresponsive &&
        now - watch_.ping_sent >= ping_interval() * kEnginePingLimitIntervals) {
        watch_.unresponsive = true;
        warning("ks-engine control thread not answering");
    }

    if (now < watch_.next_poll) {
        return;
    }
    watch_.next_poll = now + heartbeat_poll();

    // The heartbeat: a read that fails is tried again at the next poll.
    if (const std::optional<ipc::EngineStatus> status = read_status()) {
        if (status->heartbeat != 0 && !watch_.beating) {
            watch_.beating = true;
            watch_.last_heartbeat = status->heartbeat;
            watch_.heartbeat_changed = now;
        } else if (status->heartbeat != watch_.last_heartbeat) {
            watch_.last_heartbeat = status->heartbeat;
            watch_.heartbeat_changed = now;
        }
    }
    // Until the heartbeat has started, there's nothing to stand still.
    if (watch_.beating && now - watch_.heartbeat_changed >= hang_timeout()) {
        kill_hung_engine(
            std::format("heartbeat stopped for {} ms", ms(now - watch_.heartbeat_changed)));
        return;
    }
    // An engine whose control thread doesn't answer is killed only once no
    // deck plays: that would interrupt audio that still works.
    if (watch_.unresponsive && !any_deck_playing()) {
        kill_hung_engine(std::format("control thread not answering for {} ms and no deck is "
                                     "playing",
                                     ms(now - watch_.ping_sent)));
    }
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

int Supervisor::poll_wait() const {
    // While the engine runs, its log ring is read at least every
    // kLogDrainInterval (specs/logs/spec.md requirement 14).
    int timeout = poll_timeout(deadline_);
    if (engine_pid_ > 0) {
        const auto drain = static_cast<int>(kLogDrainInterval.count());
        timeout = timeout == -1 ? drain : std::min(timeout, drain);
        // And the watch's poll (specs/hang-detection/spec.md requirement 2).
        if (watch_.watching) {
            const auto watch = static_cast<int>(heartbeat_poll().count());
            timeout = std::min(timeout, watch);
        }
    }
    return timeout;
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
    const std::size_t polled_clients = clients_.size();
    for (const Client& client : clients_) {
        polled.push_back(
            {.fd = client.connection.socket(), .events = out_if(client.connection), .revents = 0});
    }
    // Then each worker with an open connection. Loads are only erased at the
    // end of a step, so these pointers stay valid through it.
    std::vector<Load*> polled_loads;
    for (const std::unique_ptr<Load>& load : loads_) {
        if (load->worker) {
            polled.push_back(
                {.fd = load->worker->socket(), .events = out_if(*load->worker), .revents = 0});
            polled_loads.push_back(load.get());
        }
    }
    const int timeout = poll_wait();
    const int ready = ::poll(polled.data(), static_cast<nfds_t>(polled.size()), timeout);
    if (ready == -1 && errno != EINTR) {
        error(std::format("poll failed: {}",
                          std::error_code{errno, std::system_category()}.message()));
    }
    drain_engine_log();
    drain_worker_logs();

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
    for (std::size_t i = 0; i < polled_clients; ++i) {
        if (const short events = polled[3 + i].revents; events != 0) {
            handle_client(clients_[i], events);
        }
    }
    for (std::size_t i = 0; i < polled_loads.size(); ++i) {
        if (const short events = polled[3 + polled_clients + i].revents; events != 0) {
            handle_worker(*polled_loads[i], events);
        }
    }
    std::erase_if(clients_, [](const Client& client) { return client.dropped; });
    // Closes each finished load's segment, and frees what it counted.
    std::erase_if(loads_, [](const std::unique_ptr<Load>& load) { return load->over(); });
    watch_engine();
    return deadline_passed();
}

std::optional<int> Supervisor::handle_signals() {
    for (const int signal : signals_->take()) {
        if (signal != SIGCHLD) {
            begin_stop(std::format("got signal {}", signal));
            continue;
        }
        reap_workers();
        if (engine_pid_ <= 0) {
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
    stop_watch();
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
        if (!had_ready_) {
            error(std::format("ks-engine {} before it was ready", how));
            return kExitStartFailed;
        }
        error(std::format("ks-engine {}", how));
        return engine_gone();
    case Phase::Running:
        error(std::format("ks-engine {}", how));
        return engine_gone();
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
    // Done only once every worker has ended too, by the stop deadline.
    stop_workers(deadline_.value_or(Clock::now()));
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
    if (std::holds_alternative<ipc::msg::Ready>(message.body) && phase_ == Phase::Starting &&
        had_ready_) {
        info("ks-engine is ready");
        phase_ = Phase::Running;
        deadline_.reset();
        start_watch();
        info(std::format(
            "ks-engine restarted in {} ms",
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - reaped_).count()));
        reattach_rings();
        return;
    }
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
        had_ready_ = true;
        phase_ = Phase::Running;
        deadline_.reset();
        start_watch();
        return;
    }
    if (std::holds_alternative<ipc::msg::Pong>(message.body)) {
        pong(id);
    } else if (std::holds_alternative<ipc::msg::Ping>(message.body)) {
        send_to_engine(ipc::Message{.request_id = id, .body = ipc::msg::Pong{}});
    } else if (const auto* const error = std::get_if<ipc::msg::Error>(&message.body)) {
        if (!attach_refused(id, *error) && !detach_refused(id, *error) &&
            !load_track_refused(id, *error) && !unload_refused(id, *error)) {
            warning(std::format("ks-engine reported an error: {}", error->text));
        }
    } else if (handle_engine_track_message(message)) {
        // Done.
    } else if (const auto* const attached = std::get_if<ipc::msg::Attached>(&message.body)) {
        if (!ring_attached(id, attached->slot)) {
            send_to_engine(error_message(id, ipc::ErrorCode::NotSupported, "answers no Attach"));
        }
    } else if (std::holds_alternative<ipc::msg::Detached>(message.body)) {
        if (!ring_detached(id)) {
            send_to_engine(error_message(id, ipc::ErrorCode::NotSupported, "answers no Detach"));
        }
    } else if (!std::holds_alternative<ipc::msg::Done>(message.body)) {
        send_to_engine(error_message(id, ipc::ErrorCode::NotSupported, "not from the engine"));
    }
}

bool Supervisor::handle_engine_track_message(const ipc::Message& message) {
    const std::uint32_t id = message.request_id;
    if (std::holds_alternative<ipc::msg::TrackLoaded>(message.body)) {
        if (!track_loaded(id)) {
            send_to_engine(error_message(id, ipc::ErrorCode::NotSupported, "answers no LoadTrack"));
        }
    } else if (std::holds_alternative<ipc::msg::DeckUnloaded>(message.body)) {
        if (!deck_unloaded(id)) {
            send_to_engine(
                error_message(id, ipc::ErrorCode::NotSupported, "answers no UnloadDeck"));
        }
    } else if (const auto* const released = std::get_if<ipc::msg::TrackReleased>(&message.body)) {
        if (!track_released(released->track_id)) {
            warning(std::format("ks-engine released track {}, which it doesn't hold",
                                released->track_id));
            send_to_engine(
                error_message(id, ipc::ErrorCode::NotSupported, "no such track is held"));
        }
    } else {
        return false;
    }
    return true;
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
    if (refuse_while_restarting(client, id)) {
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
    const std::optional<std::uint32_t> slot = free_slot();
    if (!slot) {
        warning(std::format("refused client '{}' a ring: no client slot is free", client.name));
        send_to_client(client,
                       error_message(id, ipc::ErrorCode::Unavailable, "no client slot is free"));
        return;
    }
    const std::uint32_t request = next_request();
    const std::array<int, 1> fds{segment->get()};
    client.opening = true;
    attaches_.push_back(PendingAttach{.request = request,
                                      .client = client.id,
                                      .client_request = id,
                                      .slot = *slot,
                                      .reattach = false,
                                      .segment = std::move(*segment)});
    send_to_engine(ipc::Message{.request_id = request, .body = ipc::msg::Attach{.slot = *slot}},
                   fds);
}

bool Supervisor::ring_attached(std::uint32_t id, std::uint32_t slot) {
    const auto found = std::ranges::find(attaches_, id, &PendingAttach::request);
    if (found == attaches_.end()) {
        return false;
    }
    PendingAttach pending = std::move(*found);
    attaches_.erase(found);
    if (pending.reattach) {
        info(std::format("the ring in slot {} was attached again", slot));
        settle_restart();
        return true;
    }
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
    if (pending.reattach) {
        warning(std::format("ks-engine refused the ring in slot {} again: {}", pending.slot,
                            error.text));
        settle_restart();
        return true;
    }
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
    if (had_ready_) {
        // A session is running: kill it, and carry on with a new engine when
        // it's been reaped (requirement 1).
        error(std::format("ks-engine's connection {}; killing it", why));
        ::kill(engine_pid_, SIGKILL);
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
        if (client.welcomed || stopping()) {
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
    } else if (std::holds_alternative<ipc::msg::OpenRecording>(message.body)) {
        open_recording(client, id);
    } else if (const auto* const load = std::get_if<ipc::msg::LoadFile>(&message.body)) {
        load_file(client, id, *load);
    } else if (const auto* const unload = std::get_if<ipc::msg::UnloadDeck>(&message.body)) {
        unload_deck(client, id, *unload);
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
        // A new engine that hasn't said Ready has no ring to detach; it gets
        // none of a dropped client's.
        if (!restarting_ || phase_ != Phase::Starting) {
            detach_ring(client.ring->slot, std::move(client.ring->segment));
        }
        client.ring.reset();
    }
}

void Supervisor::begin_stop(std::string_view why) {
    if (phase_ == Phase::Stopping || phase_ == Phase::Terminating) {
        return;
    }
    info(std::format("stopping: {}", why));
    for (const std::unique_ptr<Load>& load : loads_) {
        refuse_load(*load, ipc::ErrorCode::NotSupported, "stopping");
        signal_worker(*load, SIGTERM);
    }
    listener_.reset();
    // A replacement engine that's still starting hasn't read its socket: its
    // stop signal ends it (specs/engine-restart/spec.md requirement 9).
    const bool starting_again = had_ready_ && phase_ == Phase::Starting && engine_pid_ > 0;
    phase_ = Phase::Stopping;
    deadline_ = Clock::now() + kEngineStopTimeout;
    send_to_engine(ipc::Message{.request_id = next_request(), .body = ipc::msg::Stop{}});
    if (starting_again) {
        ::kill(engine_pid_, SIGTERM);
    }
}

std::optional<int> Supervisor::deadline_passed() {
    if (!deadline_ || Clock::now() < *deadline_) {
        return std::nullopt;
    }
    switch (phase_) {
    case Phase::Starting:
        error(std::format("ks-engine didn't report ready within {} ms; killing it",
                          ready_timeout().count()));
        kill_engine();
        if (!had_ready_) {
            return kExitStartFailed;
        }
        return engine_gone();
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

// ---- Loading tracks (specs/track-loading/spec.md) -----------------------------------

int Supervisor::prepare_loading() {
    if (options_->worker) {
        worker_path_ = *options_->worker;
    } else if (std::expected<std::filesystem::path, std::error_code> found =
                   default_worker_path()) {
        worker_path_ = std::move(*found);
    } else {
        error(std::format("can't find ks-worker: {}", found.error().message()));
        return kExitStartFailed;
    }
    struct stat status{};
    if (::stat(worker_path_.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
        ::access(worker_path_.c_str(), X_OK) != 0) {
        error(std::format("can't use {} as ks-worker: it isn't an executable file",
                          worker_path_.string()));
        return kExitStartFailed;
    }

    if (options_->track_memory_mib) {
        budget_ = *options_->track_memory_mib * kMib;
    } else {
        const long pages = ::sysconf(_SC_PHYS_PAGES);
        const long page_size = ::sysconf(_SC_PAGESIZE);
        if (pages <= 0 || page_size <= 0) {
            error("can't read the size of physical memory for the track memory budget");
            return kExitStartFailed;
        }
        const std::uint64_t physical =
            static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size);
        budget_ = physical / 2 / kMib * kMib;
    }
    info(std::format("track memory budget: {} MiB", budget_ / kMib));
    return kExitOk;
}

void Supervisor::load_file(Client& client, std::uint32_t id, const ipc::msg::LoadFile& request) {
    info(std::format("client '{}' asked to load {} on deck {}", client.name, request.path,
                     request.deck + 1));
    const auto refuse = [&](ipc::ErrorCode code, const std::string& why) {
        warning(
            std::format("refused client '{}' a load of {}: {}", client.name, request.path, why));
        send_to_client(client, error_message(id, code, why));
    };
    if (restarting_ && !stopping()) {
        refuse(ipc::ErrorCode::Unavailable, "ks-engine is restarting");
        return;
    }
    if (phase_ != Phase::Running) {
        refuse(ipc::ErrorCode::NotSupported, "stopping");
        return;
    }
    if (running_workers() >= kMaxWorkers) {
        refuse(ipc::ErrorCode::Unavailable,
               std::format("{} files are being decoded already", kMaxWorkers));
        return;
    }
    std::expected<ipc::Fd, std::string> file = open_track_file(request.path);
    if (!file) {
        refuse(ipc::ErrorCode::Refused, file.error());
        return;
    }

    auto load = std::make_unique<Load>();
    load->deck = request.deck;
    load->path = request.path;
    load->client = client.id;
    load->client_name = client.name;
    load->client_request = id;
    open_worker_log();
    if (const std::expected<void, std::string> started = start_worker(*load); !started) {
        refuse(ipc::ErrorCode::Unavailable, "can't start ks-worker: " + started.error());
        return;
    }
    load->track_id = next_track_++;
    std::span{latest_}[load->deck] = load->track_id;
    info(std::format("started ks-worker (process {}) for track {}", load->worker_pid,
                     load->track_id));
    const std::array<int, 1> fds{file->get()};
    send_to_worker(*load, ipc::Message{.request_id = kProbeRequest, .body = ipc::msg::Probe{}},
                   fds);
    // The worker has its own copy of the file now; the supervisor's closes.
    loads_.push_back(std::move(load));
}

std::expected<void, std::string> Supervisor::start_worker(Load& load) {
    std::expected<ipc::Fd, std::error_code> segment =
        factory_->create(ipc::SegmentKind::Log, ipc::kLogLayoutVersion, ipc::kLogSegmentSize);
    if (!segment) {
        return std::unexpected{"can't create its log segment: " + segment.error().message()};
    }
    std::expected<ipc::Region, ipc::MapFailure> region = ipc::Region::map(
        segment->get(), ipc::SegmentKind::Log, ipc::kLogLayoutVersion, ipc::Access::ReadWrite);
    if (!region) {
        return std::unexpected{std::string{"can't map its log segment"}};
    }
    auto* const ring = region->at<ipc::LogSegment>(ipc::kSegmentHeaderSize);
    if (ring == nullptr) {
        return std::unexpected{std::string{"its log segment is too small"}};
    }
    std::expected<std::pair<ipc::Fd, ipc::Fd>, std::error_code> pair = ipc::socket_pair();
    if (!pair || !set_nonblocking_cloexec(pair->first.get())) {
        return std::unexpected{std::string{"can't create its socket"}};
    }
    const std::vector<std::string> args{"--supervisor-fd", "3", "--log-fd", "4"};
    const std::array<Inherit, 2> fds{Inherit{.parent = pair->second.get(), .child = 3},
                                     Inherit{.parent = segment->get(), .child = 4}};
    const std::expected<pid_t, std::error_code> pid = spawn(worker_path_, args, fds);
    if (!pid) {
        return std::unexpected{pid.error().message()};
    }
    load.worker_pid = *pid;
    load.worker = std::make_unique<ipc::Connection>(std::move(pair->first));
    load.log_segment = std::move(*segment);
    load.log_region = std::make_unique<ipc::Region>(std::move(*region));
    load.log_reader = std::make_unique<ipc::LogReader>(*ring);
    return {};
}

void Supervisor::refuse_load(Load& load, ipc::ErrorCode code, const std::string& why) {
    if (load.answered) {
        return;
    }
    load.answered = true;
    warning(std::format("refused client '{}' a load of {}: {}", load.client_name, load.path, why));
    if (Client* const client = find_client(load.client)) {
        send_to_client(*client, error_message(load.client_request, code, why));
    }
}

void Supervisor::handle_worker(Load& load, short events) {
    if ((events & POLLOUT) != 0 && load.worker && !load.worker->flush()) {
        close_worker(load, "couldn't be written to", true);
        return;
    }
    while (load.worker) {
        std::expected<std::optional<ipc::ReceivedFrame>, ipc::ConnectionFailure> received =
            load.worker->receive();
        if (!received) {
            if (received.error().error == ipc::ConnectionError::BadFrame) {
                send_to_worker(load, error_message(0, ipc::ErrorCode::BadMessage, "a bad frame"));
                close_worker(load, "sent a bad frame", true);
            } else {
                // It has exited, or is about to: reaping it does the rest.
                close_worker(load);
            }
            return;
        }
        const std::optional<ipc::ReceivedFrame>& frame = *received;
        if (!frame) {
            return;
        }
        const std::expected<ipc::Message, ipc::MessageError> message = ipc::decode(frame->frame);
        if (!message) {
            send_to_worker(load, error_message(frame->frame.request_id, ipc::ErrorCode::BadMessage,
                                               "a message that doesn't decode"));
            close_worker(load, "sent a message that doesn't decode", true);
            return;
        }
        handle_worker_message(load, *message);
    }
}

void Supervisor::handle_worker_message(Load& load, const ipc::Message& message) {
    const std::uint32_t id = message.request_id;
    if (std::holds_alternative<ipc::msg::Ping>(message.body)) {
        send_to_worker(load, ipc::Message{.request_id = id, .body = ipc::msg::Pong{}});
    } else if (const auto* const error = std::get_if<ipc::msg::Error>(&message.body)) {
        if (!load.probed && !load.answered && error->code == ipc::ErrorCode::Refused) {
            refuse_load(load, ipc::ErrorCode::Refused, error->text);
        } else {
            warning(std::format("track {}: ks-worker reported an error: {}", load.track_id,
                                error->text));
        }
    } else if (const auto* const probed_file = std::get_if<ipc::msg::Probed>(&message.body);
               probed_file != nullptr && id == kProbeRequest && !load.probed) {
        probed(load, *probed_file);
    } else if (const auto* const decoded_track = std::get_if<ipc::msg::Decoded>(&message.body);
               decoded_track != nullptr && id == kDecodeRequest && load.decode_sent &&
               !load.decode_ended) {
        decoded(load, *decoded_track);
    } else {
        send_to_worker(load, error_message(id, ipc::ErrorCode::NotSupported, "not now"));
    }
}

void Supervisor::probed(Load& load, const ipc::msg::Probed& probed) {
    load.probed = probed;
    if (stopping() || load.worker_pid == 0) {
        // Stopping, and its client has had its answer; or the worker has
        // ended, and this was among its last messages (worker_ended()): no
        // one would decode the track.
        close_worker(load);
        return;
    }
    if (std::span{latest_}[load.deck] != load.track_id) {
        const bool unloaded = std::span{latest_}[load.deck] == kUnloaded;
        refuse_load(load, ipc::ErrorCode::Refused,
                    unloaded ? std::format("deck {} was unloaded", load.deck + 1)
                             : std::format("a later load for deck {} replaced it", load.deck + 1));
        close_worker(load);
        return;
    }
    const std::uint64_t frames =
        probed.estimated_frames != 0 ? probed.estimated_frames : ipc::kMaxTrackFrames;
    const std::uint64_t needed = frames * ipc::kTrackFrameSize;
    const std::uint64_t used = in_use();
    if (used + needed > budget_) {
        refuse_load(load, ipc::ErrorCode::Refused,
                    std::format("the track needs {} MiB of track memory, and {} MiB of the {} MiB "
                                "budget is in use",
                                mib_up(needed), mib_up(used), budget_ / kMib));
        close_worker(load);
        return;
    }
    std::expected<ipc::Fd, std::error_code> segment =
        factory_->create(ipc::SegmentKind::Track, ipc::kTrackLayoutVersion, ipc::kTrackSegmentSize);
    if (!segment) {
        refuse_load(load, ipc::ErrorCode::Unavailable,
                    "can't create a track segment: " + segment.error().message());
        close_worker(load);
        return;
    }
    std::expected<ipc::Region, ipc::MapFailure> mapping = ipc::Region::map(
        segment->get(), ipc::SegmentKind::Track, ipc::kTrackLayoutVersion, ipc::Access::ReadWrite);
    if (!mapping) {
        refuse_load(load, ipc::ErrorCode::Unavailable, "can't map the track segment");
        close_worker(load);
        return;
    }
    load.segment = std::move(*segment);
    load.mapping = std::make_unique<ipc::Region>(std::move(*mapping));
    load.reserved = needed;

    // The engine and the worker get the segment at the same time.
    const std::array<int, 1> fds{load.segment.get()};
    load.engine = EngineHold::Waiting;
    load.engine_request = next_request();
    send_to_engine(ipc::Message{.request_id = load.engine_request,
                                .body = ipc::msg::LoadTrack{.track_id = load.track_id,
                                                            .deck = load.deck,
                                                            .sample_rate = probed.sample_rate}},
                   fds);
    load.decode_sent = true;
    send_to_worker(load,
                   ipc::Message{.request_id = kDecodeRequest,
                                .body = ipc::msg::Decode{.max_frames = ipc::kMaxTrackFrames}},
                   fds);
}

void Supervisor::decoding_ended(Load& load, std::uint64_t frames, ipc::TrackStatus status) {
    load.decode_ended = true;
    load.reserved = frames * ipc::kTrackFrameSize;
    load.ended =
        ipc::msg::DecodeEnded{.track_id = load.track_id, .frames = frames, .status = status};
    notify_decode_ended(load);
}

void Supervisor::notify_decode_ended(Load& load) {
    if (load.notified || !load.file_loaded || !load.ended) {
        return;
    }
    load.notified = true;
    if (Client* const client = find_client(load.client)) {
        send_to_client(*client, ipc::Message{.request_id = 0, .body = *load.ended});
    }
}

void Supervisor::unload_deck(Client& client, std::uint32_t id,
                             const ipc::msg::UnloadDeck& request) {
    info(std::format("client '{}' asked to unload deck {}", client.name, request.deck + 1));
    if (refuse_while_restarting(client, id)) {
        return;
    }
    if (phase_ != Phase::Running) {
        send_to_client(client, error_message(id, ipc::ErrorCode::NotSupported, "stopping"));
        return;
    }
    // A load that is still probing must not put its track on the deck later.
    std::span{latest_}[request.deck] = kUnloaded;
    const std::uint32_t request_id = next_request();
    unloads_.push_back(PendingUnload{
        .request = request_id, .client = client.id, .client_request = id, .deck = request.deck});
    send_to_engine(ipc::Message{.request_id = request_id, .body = request});
}

bool Supervisor::deck_unloaded(std::uint32_t id) {
    const auto found = std::ranges::find(unloads_, id, &PendingUnload::request);
    if (found == unloads_.end()) {
        return false;
    }
    const PendingUnload pending = *found;
    unloads_.erase(found);
    if (Client* const client = find_client(pending.client)) {
        send_to_client(*client, ipc::Message{.request_id = pending.client_request,
                                             .body = ipc::msg::DeckUnloaded{}});
    }
    return true;
}

bool Supervisor::unload_refused(std::uint32_t id, const ipc::msg::Error& error) {
    const auto found = std::ranges::find(unloads_, id, &PendingUnload::request);
    if (found == unloads_.end()) {
        return false;
    }
    const PendingUnload pending = *found;
    unloads_.erase(found);
    warning(std::format("ks-engine refused to unload deck {}: {}", pending.deck + 1, error.text));
    if (Client* const client = find_client(pending.client)) {
        send_to_client(*client, error_message(pending.client_request, ipc::ErrorCode::Unavailable,
                                              error.text));
    }
    return true;
}

void Supervisor::decoded(Load& load, const ipc::msg::Decoded& decoded) {
    decoding_ended(load, decoded.frames, decoded.status);
    switch (decoded.status) {
    case ipc::TrackStatus::Complete:
        info(std::format("track {} decoded: {} frames", load.track_id, decoded.frames));
        return;
    case ipc::TrackStatus::Truncated:
        warning(std::format("track {} was truncated at {} frames", load.track_id, decoded.frames));
        return;
    case ipc::TrackStatus::Failed:
    case ipc::TrackStatus::Decoding:
        break;
    }
    warning(std::format("track {} failed after {} frames", load.track_id, decoded.frames));
}

void Supervisor::send_to_worker(Load& load, const ipc::Message& message, std::span<const int> fds) {
    if (!load.worker) {
        return;
    }
    const std::expected<ipc::Frame, ipc::MessageError> frame = ipc::encode(message);
    if (!frame || !load.worker->send(*frame, fds) || !load.worker->flush()) {
        close_worker(load, "couldn't be written to", true);
    }
}

void Supervisor::close_worker(Load& load, std::string_view why, bool stop) {
    if (!load.worker) {
        return;
    }
    if (!why.empty()) {
        warning(std::format("track {}: ks-worker {}; closing its connection", load.track_id, why));
    }
    load.worker.reset();
    if (stop) {
        signal_worker(load, SIGTERM);
    }
}

void Supervisor::signal_worker(const Load& load, int signal) {
    if (load.worker_pid > 0) {
        ::kill(load.worker_pid, signal);
    }
}

bool Supervisor::track_loaded(std::uint32_t id) {
    const auto found = std::ranges::find_if(loads_, [id](const std::unique_ptr<Load>& load) {
        return load->engine == EngineHold::Waiting && load->engine_request == id;
    });
    if (found == loads_.end()) {
        return false;
    }
    Load& load = **found;
    load.engine = EngineHold::Holds;
    info(std::format("track {} loaded on deck {}", load.track_id, load.deck + 1));
    if (load.answered || !load.probed) {
        return true;
    }
    load.answered = true;
    if (Client* const client = find_client(load.client)) {
        send_to_client(*client,
                       ipc::Message{.request_id = load.client_request,
                                    .body = ipc::msg::FileLoaded{
                                        .track_id = load.track_id,
                                        .sample_rate = load.probed->sample_rate,
                                        .estimated_frames = load.probed->estimated_frames}});
        load.file_loaded = true;
        // Decoding may have ended before the engine answered.
        notify_decode_ended(load);
    }
    return true;
}

bool Supervisor::load_track_refused(std::uint32_t id, const ipc::msg::Error& error) {
    const auto found = std::ranges::find_if(loads_, [id](const std::unique_ptr<Load>& load) {
        return load->engine == EngineHold::Waiting && load->engine_request == id;
    });
    if (found == loads_.end()) {
        return false;
    }
    Load& load = **found;
    load.engine = EngineHold::Done;
    refuse_load(load, ipc::ErrorCode::Unavailable, error.text);
    // Nothing will play what it decodes.
    signal_worker(load, SIGTERM);
    return true;
}

bool Supervisor::track_released(std::uint64_t track_id) {
    const auto found = std::ranges::find_if(loads_, [track_id](const std::unique_ptr<Load>& load) {
        return load->track_id == track_id && load->engine == EngineHold::Holds;
    });
    if (found == loads_.end()) {
        return false;
    }
    Load& load = **found;
    load.engine = EngineHold::Done;
    info(std::format("track {} released", track_id));
    // A worker still decoding stops; the segment closes once it has ended.
    signal_worker(load, SIGTERM);
    return true;
}

void Supervisor::reap_workers() {
    for (const std::unique_ptr<Load>& load : loads_) {
        if (load->worker_pid <= 0) {
            continue;
        }
        int status = 0;
        pid_t reaped = ::waitpid(load->worker_pid, &status, WNOHANG);
        while (reaped == -1 && errno == EINTR) {
            reaped = ::waitpid(load->worker_pid, &status, WNOHANG);
        }
        if (reaped == load->worker_pid) {
            worker_ended(*load, status);
        }
    }
}

void Supervisor::worker_ended(Load& load, int status) {
    const std::string how = how_it_ended(status);
    load.worker_pid = 0;
    // Its last lines, then its log segment closes.
    drain_worker_log(load);
    load.log_reader.reset();
    load.log_region.reset();
    load.log_segment.reset();
    // Its last messages: one sent just before it exited, such as a refusal,
    // may still be unread, since signals are handled before sockets in a
    // pass of the poll loop. Once they're read, the connection closes.
    if (load.worker) {
        handle_worker(load, POLLIN);
    }
    close_worker(load);
    if (!load.probed) {
        refuse_load(load, ipc::ErrorCode::Refused,
                    std::format("ks-worker {} before it probed the file", how));
    } else if (!load.decode_sent) {
        refuse_load(load, ipc::ErrorCode::Refused,
                    std::format("ks-worker {} before it decoded the file", how));
    }
    if (load.decode_sent && load.mapping) {
        finish_track(load, how);
    }
}

void Supervisor::finish_track(Load& load, std::string_view how) {
    auto* const header = load.mapping->at<ipc::TrackHeader>(ipc::kSegmentHeaderSize);
    if (header == nullptr || !load.probed) {
        return;
    }
    const std::uint64_t capacity = std::min<std::uint64_t>(
        (load.mapping->bytes().size() - ipc::kTrackSamplesOffset) / ipc::kTrackFrameSize,
        ipc::kMaxTrackFrames);
    const std::expected<ipc::TrackProgress, ipc::TrackHeaderError> progress =
        ipc::read_track(*header, capacity);
    if (progress && progress->complete) {
        // The worker finished it itself, if without a Decoded.
        if (!load.decode_ended) {
            decoding_ended(load, progress->frames, progress->status);
        }
        return;
    }
    const std::uint64_t frames = progress ? progress->frames : 0;
    ipc::TrackWriter writer{load.mapping->bytes()};
    writer.start(load.probed->sample_rate);
    writer.finish(ipc::TrackStatus::Failed, frames);
    decoding_ended(load, frames, ipc::TrackStatus::Failed);
    error(std::format("track {}: ks-worker {} without finishing the track; it ends at frame {}",
                      load.track_id, how, frames));
}

void Supervisor::drain_worker_logs() {
    for (const std::unique_ptr<Load>& load : loads_) {
        drain_worker_log(*load);
    }
}

void Supervisor::drain_worker_log(Load& load) {
    // Every call into the reader runs inside this (see SignalsBlocked).
    const SignalsBlocked blocked;
    if (!load.log_reader || load.log_reader->corrupt()) {
        return;
    }
    const std::string prefix = std::format("track {}: ", load.track_id);
    for (std::size_t i = 0; i < ipc::kLogRingCapacity; ++i) {
        const std::optional<ipc::LogRecord> record = load.log_reader->next();
        if (!record) {
            break;
        }
        // The record is a copy; validate it before using any of it, as
        // format_log_record() does.
        const bool valid = record->level >= static_cast<std::uint64_t>(ipc::LogLevel::Error) &&
                           record->level <= static_cast<std::uint64_t>(ipc::LogLevel::Info) &&
                           record->length <= ipc::kLogTextSize &&
                           (record->flags & ~ipc::kLogTruncated) == 0;
        if (!valid) {
            worker_line(ipc::LogLevel::Warning, record->time, prefix + "[invalid log record]");
            continue;
        }
        const std::span<const std::uint8_t> bytes =
            std::span{record->text}.first(static_cast<std::size_t>(record->length));
        std::string text = prefix;
        text.append(bytes.begin(), bytes.end());
        if ((record->flags & ipc::kLogTruncated) != 0) {
            text += "…";
        }
        worker_line(static_cast<ipc::LogLevel>(record->level), record->time, text);
    }
    if (load.log_reader->corrupt()) {
        const std::string corrupt = std::format(
            "track {}: ks-worker's log ring is corrupt; it won't be read again", load.track_id);
        worker_line(ipc::LogLevel::Error, steady_now_ns(), corrupt);
        error(corrupt);
        return;
    }
    if (const std::uint64_t dropped = load.log_reader->take_dropped(); dropped > 0) {
        worker_line(ipc::LogLevel::Warning, steady_now_ns(),
                    std::format("{}{} {} dropped because its log ring was full", prefix, dropped,
                                dropped == 1 ? "line was" : "lines were"));
    }
}

void Supervisor::worker_line(ipc::LogLevel level, std::uint64_t time, std::string_view text) {
    if (worker_log_ && !worker_log_->error()) {
        static_cast<void>(worker_log_->write(time, level, text));
        const std::optional<std::error_code> failed = worker_log_->error();
        if (!failed) {
            return;
        }
        error(std::format("ks-worker.log failed: {}; the workers' lines go to standard error",
                          failed->message()));
    }
    *err_ << "ks-worker: " << ipc::format_log_line(clock_, time, level, text) << '\n' << std::flush;
}

void Supervisor::open_worker_log() {
    if (worker_log_tried_ || session_.empty()) {
        return;
    }
    worker_log_tried_ = true;
    const std::filesystem::path file = session_ / "ks-worker.log";
    std::expected<LogFile, std::error_code> log = LogFile::create(file, "ks-worker", clock_);
    if (!log) {
        warning(std::format("can't create {}: {}; the workers' lines go to standard error",
                            file.string(), log.error().message()));
        return;
    }
    worker_log_ = std::make_unique<LogFile>(std::move(*log));
}

void Supervisor::stop_workers(Clock::time_point deadline) {
    for (const std::unique_ptr<Load>& load : loads_) {
        signal_worker(*load, SIGTERM);
    }
    for (;;) {
        reap_workers();
        if (running_workers() == 0 || Clock::now() >= deadline) {
            break;
        }
        static_cast<void>(::poll(nullptr, 0, 10));
    }
    for (const std::unique_ptr<Load>& load : loads_) {
        if (load->worker_pid <= 0) {
            continue;
        }
        ::kill(load->worker_pid, SIGKILL);
        int status = 0;
        while (::waitpid(load->worker_pid, &status, 0) == -1 && errno == EINTR) {
        }
        worker_ended(*load, status);
    }
}

std::size_t Supervisor::running_workers() const {
    return static_cast<std::size_t>(std::ranges::count_if(
        loads_, [](const std::unique_ptr<Load>& load) { return load->worker_pid > 0; }));
}

std::uint64_t Supervisor::in_use() const {
    std::uint64_t used = 0;
    for (const std::unique_ptr<Load>& load : loads_) {
        used += load->reserved;
    }
    return used;
}

/// The program `name` in the directory that holds this process's executable.
std::expected<std::filesystem::path, std::error_code> next_to_self(std::string_view name) {
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
    return self.parent_path() / name;
}

} // namespace

std::expected<std::filesystem::path, std::error_code> default_engine_path() {
    return next_to_self("ks-engine");
}

std::expected<std::filesystem::path, std::error_code> default_worker_path() {
    return next_to_self("ks-worker");
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
