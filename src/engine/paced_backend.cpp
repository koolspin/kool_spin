// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/paced_backend.h>

#include <ks/core/realtime.h>
#include <ks/engine/block.h>
#include <ks/engine/interleave.h>

#include <pthread.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#elifdef __linux__
#include <sched.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ks::engine {

// ---- SimulatedDevice ----------------------------------------------------------

void SimulatedDevice::unplug(Loss loss) noexcept {
    // In this order, so a backend that reads present() and then unplugs()
    // (acquire) and sees either change also sees the count and the loss.
    last_loss_.store(loss, std::memory_order_relaxed);
    unplugs_.fetch_add(1, std::memory_order_release);
    present_.store(false, std::memory_order_release);
}

void SimulatedDevice::plug_in() noexcept {
    present_.store(true, std::memory_order_release);
}

bool SimulatedDevice::present() const noexcept KS_NONBLOCKING {
    return present_.load(std::memory_order_acquire);
}

std::uint64_t SimulatedDevice::unplugs() const noexcept KS_NONBLOCKING {
    return unplugs_.load(std::memory_order_acquire);
}

Loss SimulatedDevice::last_loss() const noexcept KS_NONBLOCKING {
    return last_loss_.load(std::memory_order_relaxed);
}

// ---- The paced backend ----------------------------------------------------------

namespace {

using Clock = std::chrono::steady_clock;

/// Asks for real-time priority for the calling thread, as ADR 0003 §15 says.
/// Returns whether it was granted.
bool request_realtime_priority([[maybe_unused]] std::chrono::nanoseconds period) {
#ifdef __linux__
    // Below PipeWire's data threads (88), and needs RLIMIT_RTPRIO.
    constexpr int kPriority = 70;
    sched_param param{};
    param.sched_priority = kPriority;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
#elifdef __APPLE__
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0) {
        return false;
    }
    // Mach absolute time units are numer/denom nanoseconds.
    const auto ticks = [&timebase](std::int64_t nanoseconds) {
        const auto value =
            static_cast<std::uint64_t>(nanoseconds) * timebase.denom / timebase.numer;
        return static_cast<std::uint32_t>(
            std::min<std::uint64_t>(value, std::numeric_limits<std::uint32_t>::max()));
    };
    thread_time_constraint_policy_data_t policy{};
    policy.period = ticks(period.count());
    policy.computation = ticks(period.count() / 2);
    policy.constraint = ticks(period.count());
    policy.preemptible = TRUE;
    return thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                             reinterpret_cast<thread_policy_t>(&policy),
                             THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
#else
    return false;
#endif
}

class PacedBackendImpl final : public PacedBackend {
public:
    PacedBackendImpl(RenderCore& core, const PacedConfig& config)
        : core_{&core}, config_{config}, period_buffer_(config.period_frames * config.channels),
          scratch_storage_(kMaxOutputChannels * kMaxBlockFrames),
          recording_(config.record_frames * config.channels),
          opened_unplugs_{config.device != nullptr ? config.device->unplugs() : 0},
          info_{
              .backend = "Paced",
              .device = config.device != nullptr ? "Simulated" : "None",
              .sample_rate = config.sample_rate,
              .channels = config.channels,
              .period_frames = static_cast<std::uint32_t>(config.period_frames),
          } {
        for (std::size_t c = 0; c < kMaxOutputChannels; ++c) {
            scratch_.at(c) =
                std::span{scratch_storage_}.subspan(c * kMaxBlockFrames, kMaxBlockFrames);
        }
    }

    PacedBackendImpl(const PacedBackendImpl&) = delete;
    PacedBackendImpl& operator=(const PacedBackendImpl&) = delete;
    PacedBackendImpl(PacedBackendImpl&&) = delete;
    PacedBackendImpl& operator=(PacedBackendImpl&&) = delete;

    ~PacedBackendImpl() override { stop(); }

    std::expected<void, BackendError> start() override {
        if (thread_.joinable()) {
            return {}; // already running
        }
        const Clock::time_point start_time =
            !started_before_ && config_.start_time ? *config_.start_time : Clock::now();
        started_before_ = true;
        {
            const std::scoped_lock lock{mutex_};
            stop_requested_ = false;
        }
        thread_ = std::thread{[this, start_time] { run(start_time); }};
        return {};
    }

    void stop() noexcept override {
        if (!thread_.joinable()) {
            return;
        }
        {
            const std::scoped_lock lock{mutex_};
            stop_requested_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }

    [[nodiscard]] const StreamInfo& info() const noexcept override { return info_; }

    [[nodiscard]] bool lost() const noexcept override {
        return lost_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t callbacks() const noexcept override {
        return callbacks_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::span<const float> recording() const noexcept override {
        return std::span{recording_}.first(recorded_frames_ * config_.channels);
    }

    [[nodiscard]] Priority priority() const noexcept override {
        return priority_.load(std::memory_order_relaxed);
    }

private:
    /// The thread: renders each period at its host time, catching up when
    /// behind, until stopped or its device goes away.
    void run(Clock::time_point start_time) {
        const std::chrono::nanoseconds period =
            frames_to_duration(config_.period_frames, config_.sample_rate);
        if (config_.realtime_priority) {
            priority_.store(request_realtime_priority(period) ? Priority::Granted
                                                              : Priority::Refused,
                            std::memory_order_relaxed);
        }

        std::uint64_t n = 0;
        std::unique_lock lock{mutex_};
        while (!stop_requested_) {
            if (!device_present()) {
                // Like a real stream, it doesn't come back: wait to be stopped.
                wake_.wait(lock, [this] { return stop_requested_; });
                break;
            }
            const Clock::time_point due =
                start_time + frames_to_duration(n * config_.period_frames, config_.sample_rate);
            if (Clock::now() < due) {
                wake_.wait_until(lock, due, [this] { return stop_requested_; });
                continue;
            }
            lock.unlock();
            render_period(due);
            ++n;
            lock.lock();
        }
    }

    /// Whether its device is still there. If not, it records whether the
    /// loss was reported.
    bool device_present() noexcept {
        const SimulatedDevice* const device = config_.device;
        if (device == nullptr) {
            return true;
        }
        // present() before unplugs(): see SimulatedDevice::unplug().
        const bool present = device->present();
        const bool unplugged_since_opened = device->unplugs() != opened_unplugs_;
        if (present && !unplugged_since_opened) {
            return true;
        }
        if (unplugged_since_opened && device->last_loss() == Loss::Reported) {
            lost_.store(true, std::memory_order_relaxed);
        }
        return false;
    }

    /// Renders one period, as a device callback would, and records it.
    void render_period(Clock::time_point host_time) noexcept KS_NONBLOCKING {
        callbacks_.fetch_add(1, std::memory_order_relaxed);
        const std::span<float> period{period_buffer_};
        static_cast<void>(render_interleaved(*core_, scratch_, period, config_.channels,
                                             config_.sample_rate, host_time, config_.driver));

        const std::size_t room = config_.record_frames - recorded_frames_;
        const std::size_t frames = std::min(room, config_.period_frames);
        if (frames > 0) {
            const std::size_t samples = frames * config_.channels;
            std::ranges::copy(
                period.first(samples),
                std::span{recording_}.subspan(recorded_frames_ * config_.channels).begin());
            recorded_frames_ += frames;
        }
    }

    RenderCore* core_;
    PacedConfig config_;
    std::vector<float> period_buffer_;
    std::vector<float> scratch_storage_;
    std::array<std::span<float>, kMaxOutputChannels> scratch_{};
    std::vector<float> recording_;
    std::size_t recorded_frames_ = 0;
    std::uint64_t opened_unplugs_;
    StreamInfo info_;

    std::atomic<bool> lost_{false};
    std::atomic<std::uint64_t> callbacks_{0};
    std::atomic<Priority> priority_{Priority::NotRequested};

    bool started_before_ = false;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_requested_ = false; ///< Guarded by mutex_.
};

std::unexpected<BackendError> failure(BackendError::Kind kind, std::string message) {
    return std::unexpected{BackendError{.kind = kind, .message = std::move(message)}};
}

} // namespace

std::expected<std::unique_ptr<PacedBackend>, BackendError> open_paced(RenderCore& core,
                                                                      const PacedConfig& config) {
    if (config.channels == 0) {
        return failure(BackendError::Kind::OpenFailed, "a paced backend needs at least 1 channel");
    }
    if (config.channels > kMaxOutputChannels) {
        return failure(BackendError::Kind::TooManyChannels,
                       std::format("{} channels: the engine renders at most {}", config.channels,
                                   kMaxOutputChannels));
    }
    if (config.sample_rate == 0) {
        return failure(BackendError::Kind::OpenFailed, "a paced backend needs a sample rate");
    }
    if (config.period_frames == 0 ||
        config.period_frames > std::numeric_limits<std::uint32_t>::max()) {
        return failure(BackendError::Kind::OpenFailed,
                       std::format("a period of {} frames", config.period_frames));
    }
    return std::make_unique<PacedBackendImpl>(core, config);
}

// ---- PacedDeviceSource ----------------------------------------------------------

PacedDeviceSource::PacedDeviceSource(SimulatedDevice& device, const PacedConfig& config) noexcept
    : device_{&device}, config_{config} {}

bool PacedDeviceSource::available() {
    return device_->present();
}

std::expected<std::unique_ptr<AudioBackend>, BackendError>
PacedDeviceSource::open(RenderCore& core, DriverId driver) {
    if (!device_->present()) {
        return failure(BackendError::Kind::OpenFailed, "the simulated device is unplugged");
    }
    PacedConfig config = config_;
    config.device = device_;
    config.driver = driver;
    auto opened = open_paced(core, config);
    if (!opened) {
        return std::unexpected{std::move(opened.error())};
    }
    return std::unique_ptr<AudioBackend>{std::move(*opened)};
}

} // namespace ks::engine
