// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine_app/run.h>

#include <ks/engine/engine.h>
#include <ks/engine/miniaudio_backend.h>
#include <ks/engine/output_controller.h>
#include <ks/engine/render_core.h>
#include <ks/engine_app/options.h>
#include <ks/ipc/command.h>
#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/frame.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/region.h>
#include <ks/ipc/runtime_dir.h>
#include <ks/ipc/state.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
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

namespace ks::engine_app {

namespace {

using Clock = std::chrono::steady_clock;

/// How long the engine keeps trying to send Done once it has stopped.
constexpr std::chrono::seconds kDoneTimeout{1};

// ---- Signals, through a self-pipe (requirement 11) -----------------------------

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

/// Sets FD_CLOEXEC on `fd`, and O_NONBLOCK too if `nonblocking`. False if
/// either fails.
bool set_flags(int fd, bool nonblocking) noexcept {
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg): fcntl() is variadic
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags == -1 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
        return false;
    }
    if (!nonblocking) {
        return true;
    }
    const int status = ::fcntl(fd, F_GETFL);
    return status != -1 && ::fcntl(fd, F_SETFL, status | O_NONBLOCK) != -1;
    // NOLINTEND(cppcoreguidelines-pro-type-vararg)
}

/// True if `fd` is open.
bool is_open(int fd) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl() is variadic
    return ::fcntl(fd, F_GETFD) != -1;
}

/// True if `fd` is a stream socket.
bool is_stream_socket(int fd) noexcept {
    struct stat status{};
    if (::fstat(fd, &status) != 0 || !S_ISSOCK(status.st_mode)) {
        return false;
    }
    int type = 0;
    socklen_t size = sizeof type;
    return ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) == 0 && type == SOCK_STREAM;
}

/// A self-pipe that handlers for SIGTERM, SIGINT, and SIGHUP write to.
class SignalPipe {
public:
    static std::expected<SignalPipe, std::error_code> install() {
        std::array<int, 2> ends{-1, -1};
        if (::pipe(ends.data()) != 0) {
            return std::unexpected{std::error_code{errno, std::system_category()}};
        }
        SignalPipe pipe{ipc::Fd{ends[0]}, ipc::Fd{ends[1]}};
        if (!set_flags(ends[0], true) || !set_flags(ends[1], true)) {
            return std::unexpected{std::error_code{errno, std::system_category()}};
        }
        signal_pipe.store(ends[1], std::memory_order_relaxed);
        for (const int signal : {SIGTERM, SIGINT, SIGHUP}) {
            struct sigaction action{};
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): sigaction's handler is a
            // union member
            action.sa_handler = &on_signal;
            sigemptyset(&action.sa_mask);
            if (::sigaction(signal, &action, nullptr) != 0) {
                return std::unexpected{std::error_code{errno, std::system_category()}};
            }
        }
        return pipe;
    }

    [[nodiscard]] int read_end() const noexcept { return read_.get(); }

    /// True if a signal has arrived since the last call.
    [[nodiscard]] bool signalled() const noexcept {
        bool any = false;
        std::array<unsigned char, 16> bytes{};
        for (;;) {
            const ssize_t got = ::read(read_.get(), bytes.data(), bytes.size());
            if (got > 0) {
                any = true;
                continue;
            }
            if (got == -1 && errno == EINTR) {
                continue;
            }
            return any;
        }
    }

private:
    SignalPipe(ipc::Fd read, ipc::Fd write) noexcept
        : read_{std::move(read)}, write_{std::move(write)} {}

    ipc::Fd read_;
    ipc::Fd write_;
};

/// Why the engine couldn't start: its exit status, and a one-line reason.
struct StartFailure {
    int status = kExitStartFailed;
    std::string why;
};

std::unexpected<StartFailure> failure(std::string why, int status = kExitStartFailed) {
    return std::unexpected{StartFailure{.status = status, .why = std::move(why)}};
}

/// The output's configuration from the options, with --device turned into the
/// device's key.
std::expected<engine::MiniaudioSourceConfig, StartFailure> source_config(const Options& options) {
    engine::MiniaudioSourceConfig config{
        .backends = options.backend.value_or(engine::MiniaudioBackends::System),
        .device = std::nullopt,
        .channels = options.channels.value_or(0),
        .sample_rate = options.rate.value_or(0),
        .period_frames = options.period.value_or(0),
    };
    if (options.device) {
        const auto list = engine::list_output_devices(config.backends);
        if (!list) {
            return failure("the output devices couldn't be listed: " + list.error().message);
        }
        if (*options.device >= list->devices.size()) {
            return failure(std::format("ks-engine: device {} isn't listed; the backend lists {}",
                                       *options.device, list->devices.size()),
                           kExitUsageError);
        }
        config.device = std::span{list->devices}[*options.device].key;
    }
    return config;
}

/// A client's ring that the supervisor attached (specs/client-rings/spec.md
/// requirements 6 and 7). The engine keeps the segment's descriptor and its
/// mapping until the ring has been detached.
struct AttachedRing {
    ipc::Fd segment;
    ipc::Region region;
    ipc::CommandRing* ring = nullptr;
    /// A Detach asked for it, with request ID `detach_request`.
    bool detaching = false;
    std::uint32_t detach_request = 0;
    /// Engine::detach() has taken the request; until then, it's retried.
    bool detach_given = false;
};

/// The engine process: the control thread's work (requirements 9 to 12).
class EngineProcess {
public:
    EngineProcess(const Options& options, std::ostream& err) : options_{&options}, err_{&err} {}

    int run() {
        if (const std::expected<void, StartFailure> started = start(); !started) {
            const std::string_view why = started.error().why;
            if (why.starts_with("ks-engine: ")) {
                *err_ << why << '\n' << std::flush;
            } else {
                report(why);
            }
            return started.error().status;
        }
        return loop();
    }

private:
    void report(std::string_view line) const {
        *err_ << "ks-engine: " << line << '\n' << std::flush;
    }

    std::expected<void, StartFailure> start();
    int loop();
    /// Handles what the supervisor sent. Returns the request ID of a Stop.
    std::optional<std::uint32_t> read_supervisor(short events);
    std::optional<std::uint32_t> handle(const ipc::Message& message, std::vector<ipc::Fd>& fds);
    void send(const ipc::Message& message);
    /// Answers `id` with Error Unavailable.
    void unavailable(std::uint32_t id, std::string text);
    /// Attaches the ring in `segment` (client-rings requirement 6).
    void attach_ring(std::uint32_t id, ipc::Fd segment);
    /// Starts detaching slot `slot`'s ring (client-rings requirement 7).
    void detach_ring(std::uint32_t id, std::uint32_t slot);
    /// Asks the engine again to detach each ring whose detach it couldn't
    /// take: the give ring was full.
    void retry_detaches();
    /// Collects the engine's releases. A detached ring's is unmapped, and its
    /// Detach answered.
    void collect();
    /// Closes the supervisor's socket: the engine plays on (requirement 12).
    void lose_supervisor(std::string_view why);
    /// Fades out and stops, replies Done if `stop` asked, and returns kExitOk
    /// (requirement 11).
    int stop(std::optional<std::uint32_t> stop);

    const Options* options_;
    std::ostream* err_;

    std::unique_ptr<SignalPipe> signals_;
    std::unique_ptr<ipc::LockFile> lock_;
    ipc::Fd state_fd_;
    std::unique_ptr<ipc::Region> region_;
    std::unique_ptr<engine::Engine> engine_;
    std::unique_ptr<engine::RenderCore> core_;
    std::unique_ptr<engine::MiniaudioDeviceSource> source_;
    std::unique_ptr<engine::OutputController> controller_;
    std::unique_ptr<ipc::Connection> supervisor_;
    /// Slot i's ring, while one is attached.
    std::array<std::unique_ptr<AttachedRing>, engine::kMaxClients> rings_;
};

std::expected<void, StartFailure> EngineProcess::start() {
    std::expected<SignalPipe, std::error_code> signals = SignalPipe::install();
    if (!signals) {
        return failure("can't handle signals: " + signals.error().message());
    }
    signals_ = std::make_unique<SignalPipe>(std::move(*signals));

    // 1. The descriptors.
    const int supervisor_fd = options_->supervisor_fd.value_or(-1);
    const int state_fd = options_->state_fd.value_or(-1);
    if (!is_stream_socket(supervisor_fd) || !set_flags(supervisor_fd, true)) {
        return failure(std::format("--supervisor-fd {} isn't a stream socket", supervisor_fd));
    }
    ipc::Fd supervisor{supervisor_fd};
    if (!is_open(state_fd) || !set_flags(state_fd, false)) {
        return failure(std::format("--state-fd {} isn't open", state_fd));
    }
    state_fd_ = ipc::Fd{state_fd};

    // 2. The engine lock.
    const std::filesystem::path dir = options_->runtime_dir.value_or(std::filesystem::path{});
    std::expected<ipc::LockFile, ipc::LockFile::Failure> lock =
        ipc::LockFile::acquire(dir, ipc::kEngineLockName);
    if (!lock) {
        if (lock.error().holder) {
            return failure(std::format("another engine is running (process {})",
                                       lock.error().holder.value_or(0)));
        }
        return failure("can't take the engine lock: " + lock.error().error.message());
    }
    lock_ = std::make_unique<ipc::LockFile>(std::move(*lock));

    // 3. The state segment.
    std::expected<ipc::Region, ipc::MapFailure> region =
        ipc::Region::map(state_fd_.get(), ipc::SegmentKind::EngineState,
                         ipc::kEngineStateLayoutVersion, ipc::Access::ReadWrite);
    if (!region) {
        return failure("--state-fd isn't an engine-state segment");
    }
    region_ = std::make_unique<ipc::Region>(std::move(*region));
    auto* const state = region_->at<ipc::EngineState>(ipc::kSegmentHeaderSize);
    if (state == nullptr) {
        return failure("the engine-state segment is too small");
    }

    // 4 to 6. The engine, the device source, and the output.
    engine_ = std::make_unique<engine::Engine>(*state);
    core_ = std::make_unique<engine::RenderCore>(*engine_);
    std::expected<engine::MiniaudioSourceConfig, StartFailure> config = source_config(*options_);
    if (!config) {
        return std::unexpected{std::move(config.error())};
    }
    auto source = engine::MiniaudioDeviceSource::create(*config);
    if (!source) {
        return failure("no audio backend: " + source.error().message);
    }
    source_ = std::move(*source);
    engine::OutputConfig output;
    if (options_->rate) {
        output.default_sample_rate = *options_->rate;
    }
    if (options_->channels) {
        output.default_channels = *options_->channels;
    }
    if (options_->period) {
        output.freewheel_period_frames = *options_->period;
    }
    controller_ = std::make_unique<engine::OutputController>(*core_, *source_, output);
    controller_->start();

    // 7. Ready.
    supervisor_ = std::make_unique<ipc::Connection>(std::move(supervisor));
    send(ipc::Message{.request_id = 0, .body = ipc::msg::Ready{}});
    report("ready");
    return {};
}

int EngineProcess::loop() {
    for (;;) {
        std::array<pollfd, 2> polled{
            pollfd{.fd = signals_->read_end(), .events = POLLIN, .revents = 0},
            pollfd{.fd = supervisor_ ? supervisor_->socket() : -1,
                   .events = static_cast<short>(
                       supervisor_ && supervisor_->has_queued() ? POLLIN | POLLOUT : POLLIN),
                   .revents = 0}};
        static_cast<void>(::poll(polled.data(), static_cast<nfds_t>(polled.size()),
                                 static_cast<int>(kControlInterval.count())));

        controller_->poll();
        collect();
        retry_detaches();
        if (signals_->signalled()) {
            report("got a signal");
            return stop(std::nullopt);
        }
        if (polled[1].revents != 0 && supervisor_) {
            if (const std::optional<std::uint32_t> asked = read_supervisor(polled[1].revents)) {
                return stop(asked);
            }
        }
    }
}

std::optional<std::uint32_t> EngineProcess::read_supervisor(short events) {
    if ((events & POLLOUT) != 0 && !supervisor_->flush()) {
        lose_supervisor("couldn't be written to");
        return std::nullopt;
    }
    while (supervisor_) {
        std::expected<std::optional<ipc::ReceivedFrame>, ipc::ConnectionFailure> received =
            supervisor_->receive();
        if (!received) {
            if (received.error().error == ipc::ConnectionError::BadFrame) {
                send(ipc::Message{.request_id = 0,
                                  .body = ipc::msg::Error{.code = ipc::ErrorCode::BadMessage,
                                                          .text = "a bad frame"}});
                lose_supervisor("sent a bad frame");
            } else {
                lose_supervisor("closed");
            }
            return std::nullopt;
        }
        std::optional<ipc::ReceivedFrame>& frame = *received;
        if (!frame) {
            return std::nullopt;
        }
        const std::expected<ipc::Message, ipc::MessageError> message = ipc::decode(frame->frame);
        if (!message) {
            send(ipc::Message{.request_id = frame->frame.request_id,
                              .body = ipc::msg::Error{.code = ipc::ErrorCode::BadMessage,
                                                      .text = "a message that doesn't decode"}});
            lose_supervisor("sent a message that doesn't decode");
            return std::nullopt;
        }
        if (const std::optional<std::uint32_t> asked = handle(*message, frame->fds)) {
            return asked;
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> EngineProcess::handle(const ipc::Message& message,
                                                   std::vector<ipc::Fd>& fds) {
    const std::uint32_t id = message.request_id;
    if (std::holds_alternative<ipc::msg::Stop>(message.body)) {
        return id;
    }
    if (std::holds_alternative<ipc::msg::Ping>(message.body)) {
        send(ipc::Message{.request_id = id, .body = ipc::msg::Pong{}});
    } else if (std::holds_alternative<ipc::msg::Attach>(message.body)) {
        // decode() checked the count; the connection delivers what it says.
        if (fds.size() != 1) {
            unavailable(id, "an Attach without its segment");
        } else {
            attach_ring(id, std::move(fds.front()));
        }
    } else if (const auto* const detach = std::get_if<ipc::msg::Detach>(&message.body)) {
        detach_ring(id, detach->slot);
    } else if (const auto* const error = std::get_if<ipc::msg::Error>(&message.body)) {
        report("the supervisor reported an error: " + error->text);
    } else {
        send(ipc::Message{.request_id = id,
                          .body = ipc::msg::Error{.code = ipc::ErrorCode::NotSupported,
                                                  .text = "not for the engine"}});
    }
    return std::nullopt;
}

void EngineProcess::send(const ipc::Message& message) {
    if (!supervisor_) {
        return;
    }
    const std::expected<ipc::Frame, ipc::MessageError> frame = ipc::encode(message);
    if (!frame || !supervisor_->send(*frame, {}) || !supervisor_->flush()) {
        lose_supervisor("couldn't be written to");
    }
}

void EngineProcess::unavailable(std::uint32_t id, std::string text) {
    send(ipc::Message{
        .request_id = id,
        .body = ipc::msg::Error{.code = ipc::ErrorCode::Unavailable, .text = std::move(text)}});
}

void EngineProcess::attach_ring(std::uint32_t id, ipc::Fd segment) {
    std::expected<ipc::Region, ipc::MapFailure> region =
        ipc::Region::map(segment.get(), ipc::SegmentKind::CommandRing,
                         ipc::kCommandRingLayoutVersion, ipc::Access::ReadWrite);
    if (!region) {
        unavailable(id, "the segment isn't a command ring");
        return;
    }
    auto* const ring = region->at<ipc::CommandRing>(ipc::kSegmentHeaderSize);
    if (ring == nullptr) {
        unavailable(id, "the segment is too small for a command ring");
        return;
    }
    const std::optional<std::size_t> slot = engine_->attach(*ring);
    if (!slot) {
        unavailable(id, "no client slot is free");
        return;
    }
    rings_.at(*slot) = std::make_unique<AttachedRing>(AttachedRing{.segment = std::move(segment),
                                                                   .region = std::move(*region),
                                                                   .ring = ring,
                                                                   .detaching = false,
                                                                   .detach_request = 0,
                                                                   .detach_given = false});
    report(std::format("attached a ring in slot {}", *slot));
    send(ipc::Message{.request_id = id,
                      .body = ipc::msg::Attached{.slot = static_cast<std::uint32_t>(*slot)}});
}

void EngineProcess::detach_ring(std::uint32_t id, std::uint32_t slot) {
    if (slot >= rings_.size() || !rings_.at(slot)) {
        unavailable(id, std::format("slot {} holds no ring", slot));
        return;
    }
    AttachedRing& attached = *rings_.at(slot);
    if (attached.detaching) {
        unavailable(id, std::format("slot {}'s ring is already being detached", slot));
        return;
    }
    attached.detaching = true;
    attached.detach_request = id;
    attached.detach_given = engine_->detach(slot);
}

void EngineProcess::retry_detaches() {
    for (std::size_t slot = 0; slot < rings_.size(); ++slot) {
        AttachedRing* const attached = rings_.at(slot).get();
        if (attached != nullptr && attached->detaching && !attached->detach_given) {
            attached->detach_given = engine_->detach(slot);
        }
    }
}

void EngineProcess::collect() {
    for (std::optional<engine::Released> released = engine_->collect(); released.has_value();
         released = engine_->collect()) {
        const ipc::CommandRing* const ring = released->ring;
        if (ring == nullptr) {
            continue;
        }
        for (std::size_t slot = 0; slot < rings_.size(); ++slot) {
            std::unique_ptr<AttachedRing>& attached = rings_.at(slot);
            if (attached && attached->ring == ring) {
                const std::uint32_t request = attached->detach_request;
                // Unmaps the segment and closes its descriptor.
                attached.reset();
                report(std::format("detached the ring in slot {}", slot));
                send(ipc::Message{.request_id = request, .body = ipc::msg::Detached{}});
                break;
            }
        }
    }
}

void EngineProcess::lose_supervisor(std::string_view why) {
    if (!supervisor_) {
        return;
    }
    // Send what's queued, such as a final Error, if the socket takes it now.
    [[maybe_unused]] const std::expected<void, std::error_code> flushed = supervisor_->flush();
    supervisor_.reset();
    report(std::format("the supervisor's socket {}; playing on", why));
}

int EngineProcess::stop(std::optional<std::uint32_t> stop) {
    report("stopping");
    controller_->stop();
    if (stop && supervisor_) {
        send(ipc::Message{.request_id = *stop, .body = ipc::msg::Done{}});
        const Clock::time_point deadline = Clock::now() + kDoneTimeout;
        while (supervisor_ && supervisor_->has_queued() && Clock::now() < deadline) {
            pollfd polled{.fd = supervisor_->socket(), .events = POLLOUT, .revents = 0};
            static_cast<void>(::poll(&polled, 1, 10));
            if (!supervisor_->flush()) {
                break;
            }
        }
    }
    report("stopped");
    return kExitOk;
}

} // namespace

int run(const Options& options, std::ostream& out, std::ostream& err) {
    if (options.help) {
        out << usage();
        return kExitOk;
    }
    EngineProcess process{options, err};
    return process.run();
}

} // namespace ks::engine_app
