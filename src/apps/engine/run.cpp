// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine_app/run.h>

#include <ks/engine/audio_events.h>
#include <ks/engine/engine.h>
#include <ks/engine/miniaudio_backend.h>
#include <ks/engine/output_controller.h>
#include <ks/engine/paced_backend.h>
#include <ks/engine/render_core.h>
#include <ks/engine/thread_priority.h>
#include <ks/engine_app/health.h>
#include <ks/engine_app/options.h>
#include <ks/engine_app/segment_track.h>
#include <ks/ipc/child_process.h>
#include <ks/ipc/command.h>
#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/frame.h>
#include <ks/ipc/log.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/recording.h>
#include <ks/ipc/region.h>
#include <ks/ipc/runtime_dir.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>
#include <ks/ipc/state_dir.h>
#include <ks/ipc/track_segment.h>

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
#include <deque>
#include <expected>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace ks::engine_app {

namespace {

using Clock = std::chrono::steady_clock;

/// How long the engine keeps trying to send Done once it has stopped.
constexpr std::chrono::seconds kDoneTimeout{1};

/// The first health pass comes this long after Ready, and the others this
/// far apart (specs/health-checks/spec.md requirement 11).
constexpr std::chrono::milliseconds kFirstHealthPass{100};
constexpr std::chrono::seconds kHealthInterval{1};

/// The health block's code for a thread's scheduling policy.
std::uint64_t policy_code(engine::SchedulingPolicy policy) noexcept {
    switch (policy) {
    case engine::SchedulingPolicy::Other:
        return ipc::kPolicyOther;
    case engine::SchedulingPolicy::Fifo:
        return ipc::kPolicyFifo;
    case engine::SchedulingPolicy::RoundRobin:
        return ipc::kPolicyRr;
    case engine::SchedulingPolicy::TimeConstraint:
        return ipc::kPolicyTimeConstraint;
    case engine::SchedulingPolicy::Unknown:
        break;
    }
    return ipc::kPolicyUnknown;
}

// ---- Signals (requirement 11; specs/audio-events/spec.md requirement 9) --------
//
// No handlers: the stop signals are blocked in every thread, and the control
// thread takes them on each pass of its loop. Nothing runs asynchronously, so
// no handler can run on the audio thread, or inside KS_NONBLOCKING code.

/// Why the engine couldn't start: its exit status, and a one-line reason.
struct StartFailure {
    int status = kExitStartFailed;
    std::string why;
};

std::unexpected<StartFailure> failure(std::string why, int status = kExitStartFailed) {
    return std::unexpected{StartFailure{.status = status, .why = std::move(why)}};
}

/// Why an engine doesn't start when process `holder` holds an engine lock.
std::string another_engine(std::optional<pid_t> holder) {
    return holder ? std::format("another engine is running (process {})", *holder)
                  : std::string{"another engine is running"};
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
    /// Takes the state folder's engine lock too, if it can: only another
    /// process holding it is a failure (specs/state-folder/spec.md
    /// requirements 5 to 8).
    std::expected<void, StartFailure> lock_state_dir(const std::filesystem::path& runtime_dir);
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
    /// Waits `options_->start_delay_ms`, giving up at a stop signal. Returns
    /// true if one came.
    /// Builds the engine, resuming from `state` unless --clean (requirement 5).
    void build_engine(ipc::EngineState& state);
    bool wait_start_delay();
    /// Maps each `--track` that's usable, in the order given, and says why
    /// the others are skipped (specs/engine-restart/spec.md requirement 5).
    void map_tracks();
    /// Maps `--record-fd`'s segment and starts the writer on it (requirement
    /// 16).
    std::expected<void, StartFailure> open_recording();
    /// The device source for the options: a paced one, or miniaudio's.
    std::expected<engine::DeviceSource*, StartFailure> make_source();
    /// Sends TrackReleased for each track given and not held by a deck
    /// (requirement 7).
    void release_unheld_tracks();
    int loop();
    /// Handles what the supervisor sent. Returns the request ID of a Stop.
    std::optional<std::uint32_t> read_supervisor(short events);
    std::optional<std::uint32_t> handle(const ipc::Message& message, std::vector<ipc::Fd>& fds);
    void send(const ipc::Message& message);
    /// Refuses `what` ("an Attach" or "a Detach"): answers `id` with Error
    /// Unavailable, and logs a warning.
    void unavailable(std::uint32_t id, std::string_view what, std::string text);
    /// Attaches the ring in `segment` (client-rings requirement 6).
    void attach_ring(std::uint32_t id, std::uint32_t slot, ipc::Fd segment);
    /// Starts detaching slot `slot`'s ring (client-rings requirement 7).
    void detach_ring(std::uint32_t id, std::uint32_t slot);
    /// Asks the engine again to detach each ring whose detach it couldn't
    /// take: the give ring was full.
    void retry_detaches();
    /// Collects the engine's releases. A detached ring's is unmapped, and its
    /// Detach answered. A released track is unmapped, and the supervisor told
    /// (specs/track-loading/spec.md requirement 6).
    void collect();
    /// Puts the track in `segment` on a deck, and answers `id` once the
    /// engine holds the load (specs/track-loading/spec.md requirement 4).
    void load_track(std::uint32_t id, const ipc::msg::LoadTrack& load, ipc::Fd segment);
    /// Sends `message`, and logs `line`, once the engine holds the handover
    /// it has just given: then a restarted engine resumes with it
    /// (specs/engine-restart/spec.md requirement 8, ADR 0003 §11).
    void answer_when_held(ipc::Message message, std::string line);
    /// Sends, in order, each waiting message whose handover the engine now
    /// holds.
    void send_held_messages();
    /// Tells the supervisor that the engine released `track_id`, after every
    /// answer still waiting: the supervisor takes the release of a track only
    /// once its load has been answered.
    void send_released(std::uint64_t track_id);
    /// Passes each track's progress on to the audio thread, faulting in at
    /// most kFaultInPerPass bytes in all, and logs the tracks that finish
    /// (specs/track-loading/spec.md requirements 5 and 8).
    void relay_tracks();
    /// Logs how `track` finished.
    void log_finished(const SegmentTrack& track, const TrackFinished& finished);
    /// Locks the process's memory (specs/health-checks/spec.md requirement
    /// 1), and remembers how it went and after how many devices.
    void lock_memory();
    /// Locks the memory again once another device has started, and runs a
    /// health pass when one is due (requirements 1 and 11).
    void check_health(Clock::time_point now);
    /// Runs a health pass: gathers the readings, writes the health block,
    /// and logs what changed (requirements 11 to 13).
    void health_pass(Clock::time_point now);
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
    /// Null without a state folder, or if it can't be locked.
    std::unique_ptr<ipc::LockFile> state_lock_;
    ipc::Fd state_fd_;
    std::unique_ptr<ipc::Region> region_;
    /// The tracks the engine holds. Declared before the engine, so they
    /// outlive it and everything that renders it.
    std::vector<std::unique_ptr<SegmentTrack>> tracks_;
    std::unique_ptr<engine::Engine> engine_;
    std::unique_ptr<engine::RenderCore> core_;
    std::unique_ptr<engine::MiniaudioDeviceSource> source_;
    /// With --backend paced: the device it plays on, which is never
    /// unplugged, and its source.
    engine::SimulatedDevice simulated_device_;
    std::unique_ptr<engine::PacedDeviceSource> paced_source_;
    /// The recording segment, with --record-fd, and the writer on it.
    ipc::Fd record_fd_;
    std::unique_ptr<ipc::Region> record_region_;
    std::unique_ptr<ipc::RecordingWriter> recorder_;
    std::unique_ptr<engine::OutputController> controller_;
    std::unique_ptr<ipc::Connection> supervisor_;
    /// Slot i's ring, while one is attached.
    std::array<std::unique_ptr<AttachedRing>, engine::kMaxClients> rings_;

    /// A message, and its log line, waiting until the engine holds handover
    /// number `handover`.
    struct HeldMessage {
        std::uint64_t handover = 0;
        ipc::Message message;
        std::string line;
    };
    /// In the order they're sent.
    std::deque<HeldMessage> held_messages_;

    /// The health checks (specs/health-checks/spec.md): the pass, the
    /// health block's writer, when the next pass is due, the last
    /// mlockall()'s outcome, and how many devices had started by then.
    HealthMonitor health_;
    std::optional<ipc::SeqlockWriter<ipc::HealthState>> health_writer_;
    Clock::time_point next_health_pass_;
    ProcessLock process_lock_;
    std::uint64_t locked_after_devices_ = 0;
};

std::expected<void, StartFailure>
EngineProcess::lock_state_dir(const std::filesystem::path& runtime_dir) {
    if (!options_->state_dir) {
        return {};
    }
    const std::filesystem::path& dir = *options_->state_dir;
    const auto warn = [&](std::string_view why) {
        report(ipc::LogLevel::Warning,
               std::format("can't lock the state folder {}: {}; only the runtime directory's lock "
                           "is held",
                           dir.string(), why));
    };
    // The supervisor has judged whether the folder may be on FUSE.
    if (const auto opened = ipc::open_state_directory(dir, true); !opened) {
        warn(ipc::describe(opened.error()));
        return {};
    }
    std::error_code ignored;
    if (std::filesystem::equivalent(dir, runtime_dir, ignored)) {
        return {}; // One directory, one lock.
    }
    std::expected<ipc::LockFile, ipc::LockFile::Failure> lock =
        ipc::LockFile::acquire(dir, ipc::kEngineLockName);
    if (lock) {
        state_lock_ = std::make_unique<ipc::LockFile>(std::move(*lock));
        return {};
    }
    if (lock.error().error == std::errc::resource_unavailable_try_again) {
        return failure(another_engine(lock.error().holder));
    }
    warn(lock.error().error.message());
    return {};
}

std::expected<void, StartFailure> EngineProcess::start() {
    // Before any thread exists, so that every thread inherits the mask.
    if (const std::error_code blocked = ipc::block_stop_signals(); blocked) {
        return failure("can't block signals: " + blocked.message());
    }

    // 1. The descriptors.
    const int supervisor_fd = options_->supervisor_fd.value_or(-1);
    const int state_fd = options_->state_fd.value_or(-1);
    if (!ipc::adopt_socket(supervisor_fd)) {
        return failure(std::format("--supervisor-fd {} isn't a stream socket", supervisor_fd));
    }
    ipc::Fd supervisor{supervisor_fd};
    if (!ipc::adopt_descriptor(state_fd)) {
        return failure(std::format("--state-fd {} isn't open", state_fd));
    }
    state_fd_ = ipc::Fd{state_fd};
    if (options_->log_fd) {
        const int log_fd = *options_->log_fd;
        if (!ipc::adopt_descriptor(log_fd)) {
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
            return failure(another_engine(lock.error().holder));
        }
        return failure("can't take the engine lock: " + lock.error().error.message());
    }
    lock_ = std::make_unique<ipc::LockFile>(std::move(*lock));
    if (std::expected<void, StartFailure> locked = lock_state_dir(dir); !locked) {
        return locked;
    }

    // Held in start-up, with the lock taken: what tests use to keep a
    // restarted engine from being ready.
    if (options_->start_delay_ms && wait_start_delay()) {
        return failure("stopped before it started", kExitOk);
    }

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
    // A restarted engine counts its health passes from 0 again
    // (specs/health-checks/spec.md requirement 15).
    health_writer_.emplace(state->health);
    health_writer_->write(ipc::HealthState{});

    // 4. The recording, and the tracks a restarted engine is given.
    if (options_->record_fd) {
        if (std::expected<void, StartFailure> opened = open_recording(); !opened) {
            return opened;
        }
    }
    if (!options_->clean) {
        map_tracks();
    }

    // 5 to 7. The engine, which resumes if it can, before the device opens;
    // the device source; and the output.
    build_engine(*state);
    lock_memory();
    core_ = std::make_unique<engine::RenderCore>(*engine_);
    const std::expected<engine::DeviceSource*, StartFailure> source = make_source();
    if (!source) {
        return std::unexpected{source.error()};
    }
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
    controller_ = std::make_unique<engine::OutputController>(*core_, **source, output);
    controller_->start();
    log_output();
    if (controller_->devices_started() != locked_after_devices_) {
        lock_memory();
    }

    // 7. Ready.
    supervisor_ = std::make_unique<ipc::Connection>(std::move(supervisor));
    send(ipc::Message{.request_id = 0, .body = ipc::msg::Ready{}});
    report(ipc::LogLevel::Info, "ready");
    next_health_pass_ = Clock::now() + kFirstHealthPass;
    release_unheld_tracks();
    return {};
}

void EngineProcess::build_engine(ipc::EngineState& state) {
    if (options_->clean) {
        engine_ = std::make_unique<engine::Engine>(state);
        report(ipc::LogLevel::Info, "started clean: asked to ignore the resume state");
    } else {
        std::vector<const engine::Track*> given;
        given.reserve(tracks_.size());
        for (const std::unique_ptr<SegmentTrack>& track : tracks_) {
            given.push_back(&track->track());
        }
        engine_ =
            std::make_unique<engine::Engine>(state, std::span<const engine::Track* const>{given});
        // A fresh start has nothing to say: an engine that resumed does.
        const std::optional<std::uint64_t> generation = engine_->resume_outcome().generation;
        if (generation) {
            report(ipc::LogLevel::Info, std::format("resumed from generation {}", *generation));
        }
    }
}

bool EngineProcess::wait_start_delay() {
    const Clock::time_point until =
        Clock::now() + std::chrono::milliseconds{options_->start_delay_ms.value_or(0)};
    while (Clock::now() < until) {
        if (ipc::take_stop_signal()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return false;
}

void EngineProcess::map_tracks() {
    const std::array<std::optional<int>, 4> others{options_->supervisor_fd, options_->state_fd,
                                                   options_->log_fd, options_->record_fd};
    for (const TrackOption& option : options_->tracks) {
        const auto clashes = [&option](const std::optional<int>& other) {
            return other == option.fd;
        };
        if (std::ranges::any_of(others, clashes)) {
            report(ipc::LogLevel::Warning,
                   std::format("--track {}:{}: the descriptor is one of the engine's own",
                               option.id, option.fd));
            continue;
        }
        if (!ipc::adopt_descriptor(option.fd)) {
            report(ipc::LogLevel::Warning,
                   std::format("--track {}:{}: the descriptor isn't open", option.id, option.fd));
            continue;
        }
        std::expected<std::unique_ptr<SegmentTrack>, std::string> track =
            SegmentTrack::map(ipc::Fd{option.fd}, option.id, 0);
        if (!track) {
            report(ipc::LogLevel::Warning,
                   std::format("--track {}:{}: skipped: {}", option.id, option.fd, track.error()));
            continue;
        }
        // A resumed deck needs what's decoded so far at once, or it waits
        // for the relay and starts blocks behind where it should be. Nothing
        // is playing yet, so there's no budget to keep to.
        if (const TrackPass pass = (*track)->pass(std::numeric_limits<std::size_t>::max());
            pass.finished) {
            log_finished(**track, *pass.finished);
        }
        tracks_.push_back(std::move(*track));
    }
}

std::expected<void, StartFailure> EngineProcess::open_recording() {
    const int fd = options_->record_fd.value_or(-1);
    if (!ipc::adopt_descriptor(fd)) {
        return failure(std::format("--record-fd {} isn't open", fd));
    }
    record_fd_ = ipc::Fd{fd};
    std::expected<ipc::Region, ipc::MapFailure> region =
        ipc::Region::map(record_fd_.get(), ipc::SegmentKind::Recording,
                         ipc::kRecordingLayoutVersion, ipc::Access::ReadWrite);
    if (!region) {
        return failure("--record-fd isn't a recording segment");
    }
    record_region_ = std::make_unique<ipc::Region>(std::move(*region));
    // at() checks against the size of the engine's own mapping.
    auto* const segment = record_region_->at<ipc::RecordingSegment>(ipc::kSegmentHeaderSize);
    if (segment == nullptr) {
        return failure("the recording segment is too small");
    }
    recorder_ = std::make_unique<ipc::RecordingWriter>(
        *segment, static_cast<std::uint64_t>(Clock::now().time_since_epoch().count()));
    return {};
}

std::expected<engine::DeviceSource*, StartFailure> EngineProcess::make_source() {
    if (options_->paced) {
        const engine::PacedConfig config{
            .sample_rate = options_->rate.value_or(48'000),
            .channels = options_->channels.value_or(2),
            .period_frames = options_->period.value_or(256),
            .record_to = recorder_.get(),
        };
        paced_source_ = std::make_unique<engine::PacedDeviceSource>(simulated_device_, config);
        return paced_source_.get();
    }
    std::expected<engine::MiniaudioSourceConfig, StartFailure> config = source_config(*options_);
    if (!config) {
        return std::unexpected{std::move(config.error())};
    }
    auto source = engine::MiniaudioDeviceSource::create(*config);
    if (!source) {
        return failure("no audio backend: " + source.error().message);
    }
    source_ = std::move(*source);
    return source_.get();
}

void EngineProcess::release_unheld_tracks() {
    const auto& held = engine_->resume_outcome().tracks;
    for (std::size_t i = tracks_.size(); i > 0; --i) {
        const engine::Track* const track = &tracks_.at(i - 1)->track();
        if (std::ranges::find(held, track) != held.end()) {
            continue;
        }
        const std::uint64_t track_id = tracks_.at(i - 1)->id();
        tracks_.erase(tracks_.begin() + static_cast<std::ptrdiff_t>(i - 1));
        report(ipc::LogLevel::Info, std::format("released track {}", track_id));
        send(ipc::Message{.request_id = 0, .body = ipc::msg::TrackReleased{.track_id = track_id}});
    }
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
    // For tests of hang detection: the control thread stops answering at
    // this time, and the render thread carries on.
    const std::optional<Clock::time_point> hang_at =
        options_->hang_control_after_ms
            ? std::optional{Clock::now() +
                            std::chrono::milliseconds{*options_->hang_control_after_ms}}
            : std::nullopt;
    for (;;) {
        if (hang_at && Clock::now() >= *hang_at) {
            report(ipc::LogLevel::Warning, "the control thread is hanging, as asked");
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
        }
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
        send_held_messages();
        relay_tracks();
        retry_detaches();
        check_health(Clock::now());
        if (ipc::take_stop_signal()) {
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
    } else if (const auto* const attach = std::get_if<ipc::msg::Attach>(&message.body)) {
        // decode() checked the count; the connection delivers what it says.
        if (fds.size() != 1) {
            unavailable(id, "an Attach", "an Attach without its segment");
        } else {
            attach_ring(id, attach->slot, std::move(fds.front()));
        }
    } else if (const auto* const detach = std::get_if<ipc::msg::Detach>(&message.body)) {
        detach_ring(id, detach->slot);
    } else if (const auto* const load = std::get_if<ipc::msg::LoadTrack>(&message.body)) {
        // decode() checked the count; the connection delivers what it says.
        if (fds.size() != 1) {
            unavailable(id, "a LoadTrack", "a LoadTrack without its segment");
        } else {
            load_track(id, *load, std::move(fds.front()));
        }
    } else if (const auto* const unload = std::get_if<ipc::msg::UnloadDeck>(&message.body)) {
        if (engine_->unload(unload->deck)) {
            answer_when_held(ipc::Message{.request_id = id, .body = ipc::msg::DeckUnloaded{}},
                             std::format("unloaded deck {}", unload->deck + 1));
        } else {
            unavailable(id, "an UnloadDeck", "the engine's give ring is full");
        }
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

void EngineProcess::attach_ring(std::uint32_t id, std::uint32_t slot, ipc::Fd segment) {
    if (slot >= rings_.size() || rings_.at(slot)) {
        unavailable(id, "an Attach", std::format("slot {} is in use", slot));
        return;
    }
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
    // The tail is another process's: a ring whose counters disagree is reset
    // to empty (ADR 0003 §8; specs/engine-restart/spec.md requirement 6).
    const std::uint64_t head = ipc::load_shared(ring->head, std::memory_order_acquire);
    const std::uint64_t tail = ipc::load_shared(ring->tail, std::memory_order_relaxed);
    if (head - tail > ipc::CommandRing::kCapacity) {
        ipc::store_shared(ring->tail, head, std::memory_order_release);
        report(ipc::LogLevel::Warning,
               std::format("the ring for slot {} had inconsistent counters; reset it", slot));
    }
    if (!engine_->attach_at(slot, *ring)) {
        unavailable(id, "an Attach", "the engine's give ring is full");
        return;
    }
    rings_.at(slot) = std::make_unique<AttachedRing>(AttachedRing{.segment = std::move(segment),
                                                                  .region = std::move(*region),
                                                                  .ring = ring,
                                                                  .detaching = false,
                                                                  .detach_request = 0,
                                                                  .detach_given = false});
    report(ipc::LogLevel::Info, std::format("attached a ring in slot {}", slot));
    send(ipc::Message{.request_id = id, .body = ipc::msg::Attached{.slot = slot}});
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
        if (const engine::Track* const track = released->track; track != nullptr) {
            // Each track has one reference, so its release is its last.
            const auto found = std::ranges::find_if(
                tracks_, [track](const auto& held) { return &held->track() == track; });
            if (found != tracks_.end()) {
                const std::uint64_t track_id = (*found)->id();
                // Unmaps the segment and closes its descriptor.
                tracks_.erase(found);
                send_released(track_id);
            }
            continue;
        }
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

void EngineProcess::load_track(std::uint32_t id, const ipc::msg::LoadTrack& load, ipc::Fd segment) {
    if (std::ranges::any_of(tracks_,
                            [&load](const auto& held) { return held->id() == load.track_id; })) {
        unavailable(id, "a LoadTrack", std::format("track {} is already loaded", load.track_id));
        return;
    }
    std::expected<std::unique_ptr<SegmentTrack>, std::string> track =
        SegmentTrack::map(std::move(segment), load.track_id, load.sample_rate);
    if (!track) {
        unavailable(id, "a LoadTrack", std::move(track.error()));
        return;
    }
    if (!engine_->load(load.deck, (*track)->track())) {
        unavailable(id, "a LoadTrack", "the engine's give ring is full");
        return;
    }
    tracks_.push_back(std::move(*track));
    answer_when_held(ipc::Message{.request_id = id, .body = ipc::msg::TrackLoaded{}},
                     std::format("loaded track {} on deck {}", load.track_id, load.deck + 1));
}

void EngineProcess::answer_when_held(ipc::Message message, std::string line) {
    held_messages_.push_back(HeldMessage{.handover = engine_->handovers_given(),
                                         .message = std::move(message),
                                         .line = std::move(line)});
}

void EngineProcess::send_held_messages() {
    const std::uint64_t held = engine_->handovers_held();
    while (!held_messages_.empty() && held_messages_.front().handover <= held) {
        report(ipc::LogLevel::Info, held_messages_.front().line);
        send(held_messages_.front().message);
        held_messages_.pop_front();
    }
}

void EngineProcess::send_released(std::uint64_t track_id) {
    ipc::Message message{.request_id = 0, .body = ipc::msg::TrackReleased{.track_id = track_id}};
    std::string line = std::format("released track {}", track_id);
    if (held_messages_.empty()) {
        report(ipc::LogLevel::Info, line);
        send(message);
        return;
    }
    // After every message waiting now, whichever handover released it.
    held_messages_.push_back(HeldMessage{.handover = held_messages_.back().handover,
                                         .message = std::move(message),
                                         .line = std::move(line)});
}

void EngineProcess::relay_tracks() {
    std::size_t budget = kFaultInPerPass;
    for (const std::unique_ptr<SegmentTrack>& track : tracks_) {
        if (track->finished()) {
            continue;
        }
        const TrackPass pass = track->pass(budget);
        budget -= std::min(budget, pass.faulted);
        if (pass.finished) {
            log_finished(*track, *pass.finished);
        }
    }
}

void EngineProcess::log_finished(const SegmentTrack& track, const TrackFinished& finished) {
    if (finished.corrupt) {
        report(ipc::LogLevel::Warning,
               std::format("track {}'s header is corrupt: {}; it ends at frame {}", track.id(),
                           *finished.corrupt, finished.frames));
        return;
    }
    switch (finished.status) {
    case ipc::TrackStatus::Complete:
        report(ipc::LogLevel::Info,
               std::format("track {} decoded: {} frames", track.id(), finished.frames));
        return;
    case ipc::TrackStatus::Truncated:
        report(ipc::LogLevel::Warning,
               std::format("track {} was truncated at {} frames", track.id(), finished.frames));
        return;
    case ipc::TrackStatus::Failed:
    case ipc::TrackStatus::Decoding:
        break;
    }
    report(ipc::LogLevel::Warning,
           std::format("track {} failed after {} frames", track.id(), finished.frames));
}

void EngineProcess::lock_memory() {
    locked_after_devices_ = controller_ ? controller_->devices_started() : 0;
    process_lock_ = lock_process_memory();
}

void EngineProcess::check_health(Clock::time_point now) {
    // The device's thread and buffers are new mappings since the last lock.
    if (controller_->devices_started() != locked_after_devices_) {
        lock_memory();
    }
    if (now >= next_health_pass_) {
        health_pass(now);
        next_health_pass_ = now + kHealthInterval;
    }
}

void EngineProcess::health_pass(Clock::time_point now) {
    HealthReadings readings;
    if (const std::optional<engine::ThreadScheduling> scheduling =
            controller_->driver_scheduling()) {
        readings.policy = policy_code(scheduling->policy);
        readings.priority = static_cast<std::uint64_t>(std::max(scheduling->priority, 0));
    }
    readings.rtprio_limit = rtprio_limit();
    readings.memlock_limit = memlock_limit();
    readings.process_lock = process_lock_.state;
    readings.process_lock_error = process_lock_.error;
    for (const std::unique_ptr<SegmentTrack>& track : tracks_) {
        readings.locked_bytes += track->locked_bytes();
        readings.unlocked_bytes += track->unlocked_bytes();
    }
    const std::filesystem::path root = options_->sysfs_root.value_or(std::filesystem::path{"/sys"});
    readings.power_profile = read_power_profile(root);
    readings.power_source = read_power_source(root);
    readings.peak_load = engine_->take_peak_load();

    const std::vector<engine::EventLine> lines = health_.pass(now, readings);
    if (health_writer_) {
        health_writer_->write(health_.state());
    }
    for (const engine::EventLine& line : lines) {
        report(line.level, line.text);
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
