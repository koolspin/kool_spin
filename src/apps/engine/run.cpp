// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine_app/run.h>

#include <ks/engine/audio_events.h>
#include <ks/engine/engine.h>
#include <ks/engine/miniaudio_backend.h>
#include <ks/engine/output_controller.h>
#include <ks/engine/render_core.h>
#include <ks/engine_app/options.h>
#include <ks/ipc/command.h>
#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/frame.h>
#include <ks/ipc/log.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/region.h>
#include <ks/ipc/runtime_dir.h>
#include <ks/ipc/state.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
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

// ---- Signals (requirement 11; specs/audio-events/spec.md requirement 9) --------
//
// No handlers: the stop signals are blocked in every thread, and the control
// thread takes them on each pass of its loop. Nothing runs asynchronously, so
// no handler can run on the audio thread, or inside KS_NONBLOCKING code.

/// The signals that stop the engine.
constexpr std::array<int, 3> kStopSignals{SIGTERM, SIGINT, SIGHUP};

/// kStopSignals, as a set.
sigset_t stop_signals() noexcept {
    sigset_t set{};
    static_cast<void>(sigemptyset(&set));
    for (const int signal : kStopSignals) {
        static_cast<void>(sigaddset(&set, signal));
    }
    return set;
}

/// Blocks the stop signals in the calling thread, and so in every thread it
/// creates from then on. Returns the error, if any. The mask stays until the
/// process exits: unblocked, a second signal sent during the fade-out would
/// end the engine by its default action.
std::error_code block_stop_signals() noexcept {
    const sigset_t set = stop_signals();
    return std::error_code{::pthread_sigmask(SIG_BLOCK, &set, nullptr), std::system_category()};
}

/// Takes a pending stop signal, if there is one. True if it took one.
bool take_stop_signal() noexcept {
    sigset_t pending{};
    static_cast<void>(sigemptyset(&pending));
    if (::sigpending(&pending) != 0) {
        return false;
    }
    if (!std::ranges::any_of(kStopSignals,
                             [&](int signal) { return sigismember(&pending, signal) == 1; })) {
        return false;
    }
    // One is pending, and every thread blocks it, so this returns at once.
    const sigset_t set = stop_signals();
    int taken = 0;
    static_cast<void>(::sigwait(&set, &taken));
    return true;
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
            // A starting failure goes to standard error, and to the log too
            // if it's open (specs/logs/spec.md requirement 18).
            std::string_view why = started.error().why;
            if (why.starts_with(kPrefix)) {
                why.remove_prefix(kPrefix.size());
            }
            *err_ << kPrefix << why << '\n' << std::flush;
            if (log_) {
                log_->write(ipc::LogLevel::Error, why);
            }
            return started.error().status;
        }
        return loop();
    }

private:
    static constexpr std::string_view kPrefix = "ks-engine: ";

    /// One report: a line in the log, or on standard error without --log-fd
    /// (specs/logs/spec.md requirement 18).
    void report(ipc::LogLevel level, std::string_view line) {
        if (log_) {
            log_->write(level, line);
        } else {
            *err_ << kPrefix << line << '\n' << std::flush;
        }
    }

    std::expected<void, StartFailure> start();
    /// Maps --log-fd's segment and starts the log on it (specs/logs/spec.md
    /// requirement 17).
    std::expected<void, StartFailure> open_log();
    /// Logs the output's state if it has changed to playing or freewheeling
    /// since the last line about it (specs/logs/spec.md requirement 18).
    void log_output();
    /// Takes every event the audio thread recorded, and the dropped count,
    /// and logs the lines EventLines makes of them, with summaries for the
    /// windows that have ended, or, when `stopping`, for every window that
    /// counted anything (specs/audio-events/spec.md requirement 8).
    void log_events(bool stopping);
    int loop();
    /// Handles what the supervisor sent. Returns the request ID of a Stop.
    std::optional<std::uint32_t> read_supervisor(short events);
    std::optional<std::uint32_t> handle(const ipc::Message& message, std::vector<ipc::Fd>& fds);
    void send(const ipc::Message& message);
    /// Refuses `what` ("an Attach" or "a Detach"): answers `id` with Error
    /// Unavailable, and logs a warning.
    void unavailable(std::uint32_t id, std::string_view what, std::string text);
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
    /// (requirement 11). `why` is for the log.
    int stop(std::optional<std::uint32_t> stop, std::string_view why);

    const Options* options_;
    std::ostream* err_;

    /// The log segment, its mapping, and the writer on it, with --log-fd.
    /// The engine keeps them until it exits.
    ipc::Fd log_fd_;
    std::unique_ptr<ipc::Region> log_region_;
    std::unique_ptr<ipc::LogWriter> log_;
    /// The output state in the last line logged about it.
    engine::OutputState logged_output_ = engine::OutputState::Stopped;
    /// Turns the audio thread's events into lines.
    engine::EventLines event_lines_;

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
    // Before any thread exists, so that every thread inherits the mask.
    if (const std::error_code blocked = block_stop_signals(); blocked) {
        return failure("can't block signals: " + blocked.message());
    }

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
    if (options_->log_fd) {
        const int log_fd = *options_->log_fd;
        if (!is_open(log_fd) || !set_flags(log_fd, false)) {
            return failure(std::format("--log-fd {} isn't open", log_fd));
        }
        log_fd_ = ipc::Fd{log_fd};
        if (std::expected<void, StartFailure> opened = open_log(); !opened) {
            return opened;
        }
    }

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
    log_output();

    // 7. Ready.
    supervisor_ = std::make_unique<ipc::Connection>(std::move(supervisor));
    send(ipc::Message{.request_id = 0, .body = ipc::msg::Ready{}});
    report(ipc::LogLevel::Info, "ready");
    return {};
}

std::expected<void, StartFailure> EngineProcess::open_log() {
    std::expected<ipc::Region, ipc::MapFailure> region = ipc::Region::map(
        log_fd_.get(), ipc::SegmentKind::Log, ipc::kLogLayoutVersion, ipc::Access::ReadWrite);
    if (!region) {
        return failure("--log-fd isn't a log segment");
    }
    log_region_ = std::make_unique<ipc::Region>(std::move(*region));
    // at() checks against the size of the engine's own mapping.
    auto* const segment = log_region_->at<ipc::LogSegment>(ipc::kSegmentHeaderSize);
    if (segment == nullptr) {
        return failure("the log segment is too small");
    }
    log_ = std::make_unique<ipc::LogWriter>(*segment);
    return {};
}

void EngineProcess::log_output() {
    const engine::OutputState state = controller_->state();
    if (state == logged_output_) {
        return;
    }
    if (state == engine::OutputState::Playing) {
        report(ipc::LogLevel::Info,
               std::format("output playing on a device at {} Hz, {} channels",
                           controller_->device_sample_rate(), controller_->device_channels()));
    } else if (state == engine::OutputState::Freewheeling) {
        // The first line about the output: nothing has played yet.
        report(ipc::LogLevel::Warning, logged_output_ == engine::OutputState::Stopped
                                           ? "output freewheeling: no device opened"
                                           : "output freewheeling: the device was lost");
    } else {
        return;
    }
    logged_output_ = state;
}

void EngineProcess::log_events(bool stopping) {
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    std::vector<engine::EventLine> lines;
    const auto keep = [&lines](std::vector<engine::EventLine> more) {
        lines.insert(lines.end(), std::make_move_iterator(more.begin()),
                     std::make_move_iterator(more.end()));
    };
    while (const std::optional<engine::AudioEvent> event = engine_->take_event()) {
        keep(event_lines_.add(*event, now));
    }
    keep(event_lines_.dropped(engine_->dropped_events(), now));
    keep(stopping ? event_lines_.flush(now) : event_lines_.tick(now));
    for (const engine::EventLine& line : lines) {
        report(line.level, line.text);
    }
}

int EngineProcess::loop() {
    for (;;) {
        // The supervisor's socket, if it's still open. Signals don't wake
        // the poll; they wait for the next pass, at most kControlInterval
        // away.
        pollfd polled{.fd = supervisor_ ? supervisor_->socket() : -1,
                      .events = static_cast<short>(
                          supervisor_ && supervisor_->has_queued() ? POLLIN | POLLOUT : POLLIN),
                      .revents = 0};
        static_cast<void>(::poll(&polled, 1, static_cast<int>(kControlInterval.count())));

        controller_->poll();
        log_output();
        log_events(false);
        collect();
        retry_detaches();
        if (take_stop_signal()) {
            return stop(std::nullopt, "got a signal");
        }
        if (polled.revents != 0 && supervisor_) {
            if (const std::optional<std::uint32_t> asked = read_supervisor(polled.revents)) {
                return stop(asked, "the supervisor asked");
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
            unavailable(id, "an Attach", "an Attach without its segment");
        } else {
            attach_ring(id, std::move(fds.front()));
        }
    } else if (const auto* const detach = std::get_if<ipc::msg::Detach>(&message.body)) {
        detach_ring(id, detach->slot);
    } else if (const auto* const error = std::get_if<ipc::msg::Error>(&message.body)) {
        report(ipc::LogLevel::Warning, "the supervisor reported an error: " + error->text);
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

void EngineProcess::unavailable(std::uint32_t id, std::string_view what, std::string text) {
    report(ipc::LogLevel::Warning, std::format("refused {}: {}", what, text));
    send(ipc::Message{
        .request_id = id,
        .body = ipc::msg::Error{.code = ipc::ErrorCode::Unavailable, .text = std::move(text)}});
}

void EngineProcess::attach_ring(std::uint32_t id, ipc::Fd segment) {
    std::expected<ipc::Region, ipc::MapFailure> region =
        ipc::Region::map(segment.get(), ipc::SegmentKind::CommandRing,
                         ipc::kCommandRingLayoutVersion, ipc::Access::ReadWrite);
    if (!region) {
        unavailable(id, "an Attach", "the segment isn't a command ring");
        return;
    }
    auto* const ring = region->at<ipc::CommandRing>(ipc::kSegmentHeaderSize);
    if (ring == nullptr) {
        unavailable(id, "an Attach", "the segment is too small for a command ring");
        return;
    }
    const std::optional<std::size_t> slot = engine_->attach(*ring);
    if (!slot) {
        unavailable(id, "an Attach", "no client slot is free");
        return;
    }
    rings_.at(*slot) = std::make_unique<AttachedRing>(AttachedRing{.segment = std::move(segment),
                                                                   .region = std::move(*region),
                                                                   .ring = ring,
                                                                   .detaching = false,
                                                                   .detach_request = 0,
                                                                   .detach_given = false});
    report(ipc::LogLevel::Info, std::format("attached a ring in slot {}", *slot));
    send(ipc::Message{.request_id = id,
                      .body = ipc::msg::Attached{.slot = static_cast<std::uint32_t>(*slot)}});
}

void EngineProcess::detach_ring(std::uint32_t id, std::uint32_t slot) {
    if (slot >= rings_.size() || !rings_.at(slot)) {
        unavailable(id, "a Detach", std::format("slot {} holds no ring", slot));
        return;
    }
    AttachedRing& attached = *rings_.at(slot);
    if (attached.detaching) {
        unavailable(id, "a Detach", std::format("slot {}'s ring is already being detached", slot));
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
                report(ipc::LogLevel::Info, std::format("detached the ring in slot {}", slot));
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
    report(ipc::LogLevel::Warning, std::format("the supervisor's socket {}; playing on", why));
}

int EngineProcess::stop(std::optional<std::uint32_t> stop, std::string_view why) {
    report(ipc::LogLevel::Info, std::format("stopping: {}", why));
    controller_->stop();
    // The output is closed: the audio thread records nothing more.
    log_events(true);
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
    report(ipc::LogLevel::Info, "stopped");
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
