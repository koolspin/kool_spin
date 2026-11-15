// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the processes acceptance tests: the programs under test, starting
// and waiting for them, reading the engine's state, talking to the supervisor
// and the engine, and reading the probe's reports.

#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/frame.h>
#include <ks/ipc/listener.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/region.h>
#include <ks/ipc/runtime_dir.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>
#include <ks/supervisor/spawn.h>

#include "protocol_support.h"
#include "segments_support.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace ks::processes::test {

/// The programs under test, and the probe.
inline constexpr std::string_view kKoolSpinPath = KS_KOOL_SPIN_PATH;
inline constexpr std::string_view kEnginePath = KS_ENGINE_PATH;
inline constexpr std::string_view kProbePath = KS_PROBE_PATH;

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

/// The default wait for anything to happen.
inline constexpr Milliseconds kWait{5'000};

/// The engine options every test passes: miniaudio's null backend.
inline std::vector<std::string> null_backend() {
    return {"--backend", "null"};
}

/// How a process ended, as a shell reports it: its exit status, or 128 plus
/// the signal that killed it.
inline int outcome_of(int status) noexcept {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

/// Calls `done` every 10 ms until it returns true, or `timeout` passes. True
/// if it returned true.
template <typename Done>
bool eventually(Done done, Milliseconds timeout = kWait) {
    const Clock::time_point deadline = Clock::now() + timeout;
    for (;;) {
        if (done()) {
            return true;
        }
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(Milliseconds{10});
    }
}

/// True if a process with ID `pid` exists, as a zombie or otherwise.
inline bool exists(pid_t pid) noexcept {
    return pid > 0 && (::kill(pid, 0) == 0 || errno != ESRCH);
}

/// A child process. If it's still running when destroyed, it's killed with
/// SIGKILL and reaped, so no test leaves it behind.
class Process {
public:
    Process() = default;
    explicit Process(pid_t pid) noexcept : pid_{pid} {}
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&& other) noexcept
        : pid_{std::exchange(other.pid_, 0)}, outcome_{other.outcome_} {}
    Process& operator=(Process&& other) noexcept {
        if (this != &other) {
            kill_now();
            pid_ = std::exchange(other.pid_, 0);
            outcome_ = other.outcome_;
        }
        return *this;
    }

    ~Process() { kill_now(); }

    /// Its process ID; 0 if it never started.
    [[nodiscard]] pid_t pid() const noexcept { return pid_; }

    /// True until it has exited. Reaps it once it has.
    [[nodiscard]] bool running() { return pid_ > 0 && !outcome_ && !reap(WNOHANG); }

    /// Waits up to `timeout` for it to exit, and returns outcome_of() its
    /// status, or nothing if it's still running.
    [[nodiscard]] std::optional<int> wait(Milliseconds timeout = kWait) {
        static_cast<void>(eventually([this] { return !running(); }, timeout));
        return outcome_;
    }

    /// Sends it `signal`.
    void signal(int signal) const noexcept {
        if (pid_ > 0 && !outcome_) {
            ::kill(pid_, signal);
        }
    }

private:
    /// Kills it with SIGKILL and reaps it, if it's still running.
    void kill_now() noexcept {
        if (pid_ > 0 && !outcome_) {
            ::kill(pid_, SIGKILL);
            static_cast<void>(reap(0));
        }
    }

    /// Calls waitpid() with `options`. True if it has exited, recording how.
    bool reap(int options) noexcept {
        int status = 0;
        pid_t result = ::waitpid(pid_, &status, options);
        while (result == -1 && errno == EINTR) {
            result = ::waitpid(pid_, &status, options);
        }
        if (result == pid_) {
            outcome_ = outcome_of(status);
            return true;
        }
        if (result == -1) {
            // Not our child any more: nothing to wait for.
            outcome_ = -1;
            return true;
        }
        return false;
    }

    pid_t pid_ = 0;
    std::optional<int> outcome_;
};

/// Starts `program` with spawn(). A Process with ID 0 if it couldn't start.
inline Process start(std::string_view program, const std::vector<std::string>& args,
                     std::span<const supervisor::Inherit> fds = {}) {
    const std::expected<pid_t, std::error_code> pid =
        supervisor::spawn(std::filesystem::path{program}, args, fds);
    return Process{pid.value_or(0)};
}

/// Who holds `<dir>/<name>.lock`: nothing if it's free, and -1 if it's held
/// by a process whose ID the file doesn't give.
inline std::optional<pid_t> lock_holder(const std::filesystem::path& dir, std::string_view name) {
    const std::expected<ipc::LockState, std::error_code> state = ipc::probe_lock(dir, name);
    if (!state || !state->held) {
        return std::nullopt;
    }
    return state->pid.value_or(-1);
}

/// When destroyed, kills with SIGKILL whatever holds the engine lock in a
/// runtime directory: an engine, or the probe standing in for one, that a
/// test left without a supervisor.
class EngineReaper {
public:
    explicit EngineReaper(std::filesystem::path dir) : dir_{std::move(dir)} {}
    EngineReaper(const EngineReaper&) = delete;
    EngineReaper& operator=(const EngineReaper&) = delete;
    EngineReaper(EngineReaper&&) = delete;
    EngineReaper& operator=(EngineReaper&&) = delete;

    ~EngineReaper() {
        if (const std::optional<pid_t> pid = lock_holder(dir_, ipc::kEngineLockName);
            pid && *pid > 0) {
            ::kill(*pid, SIGKILL);
        }
    }

private:
    std::filesystem::path dir_;
};

/// An engine-state segment, mapped read-only.
class StateView {
public:
    /// Maps `fd` as an engine-state segment, or says why it couldn't.
    static std::expected<StateView, std::string> map(int fd) {
        std::expected<ipc::Region, ipc::MapFailure> region =
            ipc::Region::map(fd, ipc::SegmentKind::EngineState, ipc::kEngineStateLayoutVersion,
                             ipc::Access::ReadOnly);
        if (!region) {
            return std::unexpected{std::string{"not an engine-state segment"}};
        }
        auto* const state = region->at<ipc::EngineState>(ipc::kSegmentHeaderSize);
        if (state == nullptr) {
            return std::unexpected{std::string{"too small for the engine's state"}};
        }
        return StateView{std::move(*region), *state};
    }

    /// The last status block published, or nothing if none was, or it
    /// couldn't be read.
    [[nodiscard]] std::optional<ipc::EngineStatus> status() const {
        const std::optional<ipc::EngineStatus> status =
            ipc::SeqlockReader<ipc::EngineStatus>{state_->status}.try_read(1'000);
        if (!status || status->heartbeat == 0) {
            return std::nullopt;
        }
        return status;
    }

    /// The status block's heartbeat; 0 if there's none yet.
    [[nodiscard]] std::uint64_t heartbeat() const {
        const std::optional<ipc::EngineStatus> current = status();
        return current ? current->heartbeat : 0;
    }

private:
    StateView(ipc::Region region, ipc::EngineState& state) noexcept
        : region_{std::move(region)}, state_{&state} {}

    ipc::Region region_;
    ipc::EngineState* state_;
};

/// True if a first heartbeat appears within 5 s, and then the heartbeat
/// increases between each check, 100 ms apart, for `duration`. Ready comes
/// before the device's first block, so a heartbeat of 0 means "not yet".
inline bool heartbeat_keeps_increasing(const StateView& view,
                                       Milliseconds duration = Milliseconds{500}) {
    std::uint64_t last = 0;
    if (!eventually([&] {
            last = view.heartbeat();
            return last != 0;
        })) {
        return false;
    }
    for (Milliseconds waited{0}; waited < duration; waited += Milliseconds{100}) {
        std::this_thread::sleep_for(Milliseconds{100});
        const std::uint64_t now = view.heartbeat();
        if (now <= last) {
            return false;
        }
        last = now;
    }
    return true;
}

/// True if the last status block published has kStatusMuted and
/// kStatusSilenced set.
inline bool silenced(const StateView& view) {
    const std::optional<ipc::EngineStatus> status = view.status();
    constexpr std::uint64_t kBoth = ipc::kStatusMuted | ipc::kStatusSilenced;
    return status && (status->flags & kBoth) == kBoth;
}

/// Sends `message` with `fds`, and flushes it. False if either fails.
inline bool send(ipc::Connection& connection, const ipc::Message& message,
                 std::span<const int> fds = {}) {
    const std::expected<ipc::Frame, ipc::MessageError> frame = ipc::encode(message);
    return frame && connection.send(*frame, fds) && connection.flush();
}

/// A message received, with its descriptors.
struct Received {
    ipc::Message message;
    std::vector<ipc::Fd> fds;
};

/// Waits up to 5 s for a frame on a non-blocking connection, and decodes it.
/// Nothing if none arrived, the connection failed, or it didn't decode.
inline std::optional<Received> receive(ipc::Connection& connection) {
    std::expected<std::optional<ipc::ReceivedFrame>, ipc::ConnectionFailure> received =
        ipc::test::receive_within(connection);
    if (!received) {
        return std::nullopt;
    }
    std::optional<ipc::ReceivedFrame>& frame = *received;
    if (!frame) {
        return std::nullopt;
    }
    std::expected<ipc::Message, ipc::MessageError> message = ipc::decode(frame->frame);
    if (!message) {
        return std::nullopt;
    }
    return Received{.message = std::move(*message), .fds = std::move(frame->fds)};
}

/// True if `received` is a message of type `T`, with request ID `id`.
template <typename T>
bool is(const std::optional<Received>& received, std::uint32_t id) {
    return received && std::holds_alternative<T>(received->message.body) &&
           received->message.request_id == id;
}

/// The code of the Error `received` is, or nothing if it isn't one.
inline std::optional<ipc::ErrorCode> error_code(const std::optional<Received>& received) {
    if (!received) {
        return std::nullopt;
    }
    const auto* const error = std::get_if<ipc::msg::Error>(&received->message.body);
    if (error == nullptr) {
        return std::nullopt;
    }
    return error->code;
}

/// What happened to a connection that should close.
struct Closing {
    bool closed = false;   ///< It closed, or failed, within 5 s.
    bool saw_done = false; ///< A Done arrived before it closed.
};

/// Reads a connection until it closes or fails, for up to 5 s, noting
/// whether a Done arrived first.
inline Closing read_until_closed(ipc::Connection& connection) {
    Closing result;
    const Clock::time_point deadline = Clock::now() + kWait;
    while (Clock::now() < deadline) {
        std::expected<std::optional<ipc::ReceivedFrame>, ipc::ConnectionFailure> received =
            connection.receive();
        if (!received) {
            result.closed = true;
            return result;
        }
        if (const std::optional<ipc::ReceivedFrame>& frame = *received; frame) {
            const std::expected<ipc::Message, ipc::MessageError> message =
                ipc::decode(frame->frame);
            if (message && std::holds_alternative<ipc::msg::Done>(message->body)) {
                result.saw_done = true;
            }
            continue;
        }
        static_cast<void>(ipc::test::wait_readable(connection.socket(), 100));
    }
    return result;
}

/// True if the connection closes, or fails, within 5 s. Messages that arrive
/// first are skipped.
inline bool closes(ipc::Connection& connection) {
    return read_until_closed(connection).closed;
}

/// A non-blocking connection to the supervisor of runtime directory `dir`,
/// or why connecting failed.
inline std::expected<ipc::Connection, std::error_code> connect(const std::filesystem::path& dir) {
    std::expected<ipc::Fd, std::error_code> socket = ipc::connect_to_supervisor(dir, ::geteuid());
    if (!socket) {
        return std::unexpected{socket.error()};
    }
    if (!ipc::test::set_nonblocking(socket->get())) {
        return std::unexpected{std::error_code{errno, std::system_category()}};
    }
    return ipc::Connection{std::move(*socket)};
}

/// Says Hello as `name`, with request ID `id`. Returns the Welcome's
/// engine-state segment, mapped read-only, or why there's none.
inline std::expected<StateView, std::string> hello(ipc::Connection& connection, std::string name,
                                                   std::uint32_t id = 1) {
    if (!send(connection,
              ipc::Message{.request_id = id, .body = ipc::msg::Hello{std::move(name)}})) {
        return std::unexpected{std::string{"couldn't send Hello"}};
    }
    const std::optional<Received> welcome = receive(connection);
    if (!welcome) {
        return std::unexpected{std::string{"no answer"}};
    }
    if (!std::holds_alternative<ipc::msg::Welcome>(welcome->message.body) ||
        welcome->message.request_id != id) {
        return std::unexpected{std::string{"an answer that isn't a Welcome"}};
    }
    if (welcome->fds.size() != 1) {
        return std::unexpected{std::string{"a Welcome without one descriptor"}};
    }
    return StateView::map(welcome->fds.front().get());
}

/// True if a Ping with request ID `id` is answered with a Pong with the same
/// ID.
inline bool pings(ipc::Connection& connection, std::uint32_t id) {
    return send(connection, ipc::Message{.request_id = id, .body = ipc::msg::Ping{}}) &&
           is<ipc::msg::Pong>(receive(connection), id);
}

/// A frame header with version 0, which every connection refuses.
inline std::vector<std::byte> bad_header() {
    std::expected<std::vector<std::byte>, ipc::FrameError> frame =
        ipc::encode_frame(static_cast<std::uint16_t>(ipc::MessageType::Ping), 1, {}, 0);
    if (!frame) {
        return {};
    }
    frame->at(0) = std::byte{0};
    frame->at(1) = std::byte{0};
    return std::move(*frame);
}

/// What the probe reports about itself (process_probe.cpp).
struct ProbeReport {
    pid_t pid = 0;
    pid_t pgid = 0;
    /// Its open descriptors, and the files they refer to.
    std::map<int, ipc::test::FileId> fds;
    /// The signals blocked in its signal mask.
    std::vector<int> blocked;
    /// "default", "ignored", or "handled", for INT, TERM, HUP, and PIPE.
    std::map<std::string, std::string> dispositions;
    /// Its arguments after argv[0].
    std::vector<std::string> args;
};

/// The bytes that `hex`, two lowercase hex digits per byte, stands for.
inline std::optional<std::string> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        return std::nullopt;
    }
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    std::string result;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int high = digit(hex[i]);
        const int low = digit(hex[i + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        result.push_back(static_cast<char>((high * 16) + low));
    }
    return result;
}

/// Reads the report the probe wrote to `path`, or says why it can't: there's
/// none yet, or it's malformed. The probe writes it whole, by renaming.
inline std::expected<ProbeReport, std::string> read_report(const std::filesystem::path& path) {
    std::ifstream in{path};
    if (!in) {
        return std::unexpected{std::string{"no report"}};
    }
    ProbeReport report;
    bool ended = false;
    std::string word;
    while (in >> word) {
        if (word == "pid") {
            in >> report.pid;
        } else if (word == "pgid") {
            in >> report.pgid;
        } else if (word == "fd") {
            int fd = -1;
            unsigned long long device = 0;
            unsigned long long inode = 0;
            in >> fd >> device >> inode;
            report.fds[fd] = ipc::test::FileId{.device = static_cast<dev_t>(device),
                                               .inode = static_cast<ino_t>(inode)};
        } else if (word == "blocked") {
            int signal = 0;
            in >> signal;
            report.blocked.push_back(signal);
        } else if (word == "disposition") {
            std::string name;
            std::string disposition;
            in >> name >> disposition;
            report.dispositions[name] = disposition;
        } else if (word == "arg") {
            std::string hex;
            in >> hex;
            // "-" is the empty argument.
            const std::optional<std::string> arg = hex == "-" ? std::string{} : from_hex(hex);
            if (!arg) {
                return std::unexpected{std::string{"a malformed argument"}};
            }
            report.args.push_back(arg.value_or(std::string{}));
        } else if (word == "end") {
            ended = true;
        } else {
            return std::unexpected{"an unknown line: " + word};
        }
        if (!in) {
            return std::unexpected{"a malformed line: " + word};
        }
    }
    if (!ended) {
        return std::unexpected{std::string{"no end"}};
    }
    return report;
}

/// Waits up to 5 s for the probe's report at `path`.
inline std::expected<ProbeReport, std::string> wait_for_report(const std::filesystem::path& path) {
    std::expected<ProbeReport, std::string> report = read_report(path);
    static_cast<void>(eventually([&] {
        report = read_report(path);
        return report.has_value();
    }));
    return report;
}

} // namespace ks::processes::test
