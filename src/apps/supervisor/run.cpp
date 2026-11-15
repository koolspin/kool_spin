// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/supervisor/run.h>

#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/fd_passing.h>
#include <ks/ipc/frame.h>
#include <ks/ipc/listener.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/runtime_dir.h>
#include <ks/ipc/segment.h>
#include <ks/ipc/state.h>
#include <ks/supervisor/options.h>
#include <ks/supervisor/spawn.h>

#include <fcntl.h>
#include <poll.h>
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

/// One client's connection.
struct Client {
    ipc::Connection connection;
    bool welcomed = false;
    std::string name;
    /// The request IDs of its Stops, to answer with Done.
    std::vector<std::uint32_t> stops;
    /// Set once it's to be closed.
    bool dropped = false;
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
    void report(std::string_view line) const {
        *err_ << "kool_spin: " << line << '\n' << std::flush;
    }

    /// Requirement 15, steps 1 to 5. Returns kExitOk, or reports and returns
    /// kExitStartFailed.
    int start();

    /// One pass of the poll loop. Returns the exit status once the
    /// supervisor is done.
    std::optional<int> step();

    std::optional<int> handle_signals();
    std::optional<int> engine_exited(int status);
    void handle_engine(short events);
    void handle_engine_message(const ipc::Message& message);
    void engine_failed(std::string_view why);
    void send_to_engine(const ipc::Message& message);
    void accept_clients();
    void handle_client(Client& client, short events);
    void handle_client_message(Client& client, const ipc::Message& message);
    void send_to_client(Client& client, const ipc::Message& message, std::span<const int> fds = {});
    void refuse_and_drop(Client& client, std::uint32_t request_id, std::string why);
    void drop(Client& client, std::string_view why);
    void begin_stop();
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

    pid_t engine_pid_ = 0;
    std::unique_ptr<ipc::Connection> engine_;
    std::uint32_t next_request_ = 1;

    std::unique_ptr<ipc::Listener> listener_;
    std::vector<Client> clients_;

    Phase phase_ = Phase::Starting;
    std::optional<Clock::time_point> deadline_;
    /// Set when the supervisor must exit with this status whatever the engine
    /// does: it's stopping the engine because it couldn't start.
    std::optional<int> forced_status_;
};

int Supervisor::start() {
    dir_ = options_->runtime_dir.value_or(ipc::default_runtime_directory_path());
    if (const auto opened = ipc::open_runtime_directory(dir_); !opened) {
        report(std::format("can't use the runtime directory {}: {}", dir_.string(),
                           opened.error().message()));
        return kExitStartFailed;
    }

    std::expected<ipc::LockFile, ipc::LockFile::Failure> lock =
        ipc::LockFile::acquire(dir_, ipc::kSupervisorLockName);
    if (!lock) {
        if (lock.error().holder) {
            report(std::format("another supervisor is running (process {})",
                               lock.error().holder.value_or(0)));
        } else {
            report(std::format("can't take the supervisor lock: {}", lock.error().error.message()));
        }
        return kExitStartFailed;
    }
    lock_ = std::make_unique<ipc::LockFile>(std::move(*lock));

    const std::expected<ipc::LockState, std::error_code> engine_lock =
        ipc::probe_lock(dir_, ipc::kEngineLockName);
    if (!engine_lock) {
        report(std::format("can't check the engine lock: {}", engine_lock.error().message()));
        return kExitStartFailed;
    }
    if (engine_lock->held) {
        report(engine_lock->pid
                   ? std::format("an engine from an earlier session is still running "
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
        report(std::format("can't find ks-engine: {}", found.error().message()));
        return kExitStartFailed;
    }

    std::expected<ipc::SegmentFactory, std::error_code> factory = ipc::SegmentFactory::open(dir_);
    if (!factory) {
        report(std::format("can't create segments: {}", factory.error().message()));
        return kExitStartFailed;
    }
    factory_ = std::make_unique<ipc::SegmentFactory>(std::move(*factory));
    std::expected<ipc::Fd, std::error_code> state =
        factory_->create(ipc::SegmentKind::EngineState, ipc::kEngineStateLayoutVersion,
                         ipc::kEngineStateSegmentSize);
    if (!state) {
        report(std::format("can't create the engine-state segment: {}", state.error().message()));
        return kExitStartFailed;
    }
    state_ = std::move(*state);

    constexpr std::array<int, 4> kSignals{SIGCHLD, SIGINT, SIGTERM, SIGHUP};
    std::expected<SignalPipe, std::error_code> signals = SignalPipe::install(kSignals);
    if (!signals) {
        report(std::format("can't handle signals: {}", signals.error().message()));
        return kExitStartFailed;
    }
    signals_ = std::make_unique<SignalPipe>(std::move(*signals));

    std::expected<std::pair<ipc::Fd, ipc::Fd>, std::error_code> pair = ipc::socket_pair();
    if (!pair || !set_nonblocking_cloexec(pair->first.get())) {
        report("can't create the engine's socket");
        return kExitStartFailed;
    }
    std::vector<std::string> args{"--supervisor-fd", "3",          "--state-fd", "4",
                                  "--runtime-dir",   dir_.string()};
    args.insert(args.end(), options_->engine_args.begin(), options_->engine_args.end());
    const std::array<Inherit, 2> fds{Inherit{.parent = pair->second.get(), .child = 3},
                                     Inherit{.parent = state_.get(), .child = 4}};
    const std::expected<pid_t, std::error_code> pid = spawn(engine, args, fds);
    if (!pid) {
        report(std::format("can't start {}: {}", engine.string(), pid.error().message()));
        return kExitStartFailed;
    }
    engine_pid_ = *pid;
    engine_ = std::make_unique<ipc::Connection>(std::move(pair->first));
    report(std::format("started ks-engine (process {})", engine_pid_));
    deadline_ = Clock::now() + kEngineReadyTimeout;
    return kExitOk;
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
    const int ready =
        ::poll(polled.data(), static_cast<nfds_t>(polled.size()), poll_timeout(deadline_));
    if (ready == -1 && errno != EINTR) {
        report(std::format("poll failed: {}",
                           std::error_code{errno, std::system_category()}.message()));
    }

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
            report(std::format("got signal {}", signal));
            begin_stop();
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
    const std::string how = how_it_ended(status);
    if (forced_status_) {
        report(std::format("ks-engine {}", how));
        return forced_status_;
    }
    switch (phase_) {
    case Phase::Starting:
        report(std::format("ks-engine {} before it was ready", how));
        return kExitStartFailed;
    case Phase::Running:
    case Phase::Terminating:
        report(std::format("ks-engine {}", how));
        return kExitEngineFailed;
    case Phase::Stopping:
        break;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        report(std::format("ks-engine {} while stopping", how));
        return kExitEngineFailed;
    }
    report("ks-engine stopped");
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
        report("ks-engine is ready");
        std::expected<ipc::Listener, std::error_code> listener = ipc::Listener::open(dir_);
        if (!listener) {
            report(std::format("can't open the supervisor socket: {}", listener.error().message()));
            forced_status_ = kExitStartFailed;
            begin_stop();
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
        report(std::format("ks-engine reported an error: {}", error->text));
    } else if (!std::holds_alternative<ipc::msg::Pong>(message.body) &&
               !std::holds_alternative<ipc::msg::Done>(message.body)) {
        send_to_engine(error_message(id, ipc::ErrorCode::NotSupported, "not from the engine"));
    }
}

void Supervisor::send_to_engine(const ipc::Message& message) {
    if (!engine_) {
        return;
    }
    const std::expected<ipc::Frame, ipc::MessageError> frame = ipc::encode(message);
    if (!frame || !engine_->send(*frame, {}) || !engine_->flush()) {
        engine_failed("couldn't be written to");
    }
}

void Supervisor::engine_failed(std::string_view why) {
    engine_.reset();
    if (phase_ == Phase::Stopping || phase_ == Phase::Terminating) {
        // It's on its way out: wait for it.
        return;
    }
    report(std::format("ks-engine's connection {}; stopping it", why));
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
                report("refused a connection from another user");
                continue;
            }
            report(std::format("can't accept a connection: {}", accepted.error().message()));
            return;
        }
        std::optional<ipc::Fd>& socket = *accepted;
        if (!socket) {
            return;
        }
        if (clients_.size() >= kMaxConnections) {
            report(std::format("refused a connection: {} are open already", kMaxConnections));
            continue;
        }
        clients_.push_back(Client{.connection = ipc::Connection{std::move(*socket)},
                                  .welcomed = false,
                                  .name = {},
                                  .stops = {},
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
                drop(client, "disconnected");
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
        report(std::format("client '{}' said Hello", client.name));
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
        report(std::format("client '{}' asked to stop", client.name));
        client.stops.push_back(id);
        begin_stop();
    } else if (const auto* const error = std::get_if<ipc::msg::Error>(&message.body)) {
        report(std::format("client '{}' reported an error: {}", client.name, error->text));
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

void Supervisor::drop(Client& client, std::string_view why) {
    if (client.dropped) {
        return;
    }
    client.dropped = true;
    report(client.name.empty() ? std::format("dropped a client: it {}", why)
                               : std::format("dropped client '{}': it {}", client.name, why));
}

void Supervisor::begin_stop() {
    if (phase_ == Phase::Stopping || phase_ == Phase::Terminating) {
        return;
    }
    report("stopping");
    listener_.reset();
    phase_ = Phase::Stopping;
    deadline_ = Clock::now() + kEngineStopTimeout;
    send_to_engine(ipc::Message{.request_id = next_request_++, .body = ipc::msg::Stop{}});
}

std::optional<int> Supervisor::deadline_passed() {
    if (!deadline_ || Clock::now() < *deadline_) {
        return std::nullopt;
    }
    switch (phase_) {
    case Phase::Starting:
        report(std::format("ks-engine didn't report ready within {} s; killing it",
                           kEngineReadyTimeout.count()));
        kill_engine();
        return kExitStartFailed;
    case Phase::Stopping:
    case Phase::Terminating:
        report(std::format("ks-engine didn't stop within {} s; killing it",
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
