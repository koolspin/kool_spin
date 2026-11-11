// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/output_controller.h>

#include <ks/engine/paced_backend.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace ks::engine {

namespace {

/// How long stop() waits for the fade out beyond the fade time.
constexpr std::chrono::milliseconds kFadeOutMargin{100};
/// How often stop() checks whether the fade out has finished.
constexpr std::chrono::milliseconds kFadeOutPoll{1};

} // namespace

OutputController::OutputController(RenderCore& core, DeviceSource& source,
                                   const OutputConfig& config) noexcept
    : core_{&core}, source_{&source}, config_{config} {}

OutputController::~OutputController() {
    stop();
}

void OutputController::start() {
    if (state_ != OutputState::Stopped) {
        return;
    }
    core_->set_muted(false);
    const Clock::time_point now = Clock::now();
    const DriverId driver = next_driver();
    if (std::unique_ptr<AudioBackend> device = open_device(driver)) {
        // Active before it starts, so its first callback renders.
        core_->activate(driver);
        if (device->start()) {
            adopt_device(std::move(device), driver, now);
            state_ = OutputState::Playing;
            return;
        }
    }
    start_freewheel(now);
    state_ = OutputState::Freewheeling;
    last_retry_ = now;
}

void OutputController::poll() {
    const Clock::time_point now = Clock::now();
    switch (state_) {
    case OutputState::Stopped:
        return;

    case OutputState::Playing: {
        const std::uint64_t frames = core_->frames_rendered();
        if (frames != last_frames_) {
            last_frames_ = frames;
            last_progress_ = now;
        }
        if (device_->lost() || now - last_progress_ >= config_.stall_timeout) {
            // Freewheel from where the device stopped, so the clock catches
            // up the time it took to notice. Only then close the device.
            start_freewheel(std::max(core_->rendered_until(), device_started_));
            close(device_);
            state_ = OutputState::Freewheeling;
            last_retry_ = now;
        }
        return;
    }

    case OutputState::Freewheeling: {
        if (now - last_retry_ < config_.retry_interval) {
            return;
        }
        last_retry_ = now;
        const DriverId driver = next_driver();
        if (std::unique_ptr<AudioBackend> device = open_device(driver); device && device->start()) {
            // Not active yet: the freewheel driver keeps rendering until the
            // device calls back.
            device_ = std::move(device);
            device_driver_ = driver;
            device_started_ = now;
            state_ = OutputState::Attaching;
        }
        return;
    }

    case OutputState::Attaching:
        if (device_->callbacks() > 0) {
            core_->activate(device_driver_);
            close(freewheel_);
            adopt_device(std::move(device_), device_driver_, device_started_);
            last_progress_ = now;
            state_ = OutputState::Playing;
        } else if (device_->lost() || now - device_started_ >= config_.stall_timeout) {
            close(device_);
            state_ = OutputState::Freewheeling;
        }
        return;
    }
}

void OutputController::stop() {
    core_->set_muted(true);
    // Let the fade out finish, if anything is rendering it.
    if (freewheel_ || device_) {
        const Clock::time_point deadline =
            Clock::now() + core_->safety_config().fade + kFadeOutMargin;
        while (!core_->silenced() && Clock::now() < deadline) {
            std::this_thread::sleep_for(kFadeOutPoll);
        }
    }
    close(freewheel_);
    close(device_);
    state_ = OutputState::Stopped;
}

OutputState OutputController::state() const noexcept {
    return state_;
}

DriverId OutputController::next_driver() noexcept {
    return DriverId{next_driver_++};
}

std::unique_ptr<AudioBackend> OutputController::open_device(DriverId driver) {
    if (!source_->available()) {
        return nullptr;
    }
    auto opened = source_->open(*core_, driver);
    return opened ? std::move(*opened) : nullptr;
}

void OutputController::adopt_device(std::unique_ptr<AudioBackend> device, DriverId driver,
                                    Clock::time_point started) {
    device_ = std::move(device);
    device_driver_ = driver;
    device_started_ = started;
    sample_rate_ = device_->info().sample_rate;
    channels_ = device_->info().channels;
    last_frames_ = core_->frames_rendered();
    last_progress_ = Clock::now();
}

void OutputController::start_freewheel(Clock::time_point start_time) {
    const DriverId driver = next_driver();
    auto opened = open_paced(
        *core_, PacedConfig{
                    .sample_rate = sample_rate_ != 0 ? sample_rate_ : config_.default_sample_rate,
                    .channels = channels_ != 0 ? channels_ : config_.default_channels,
                    .period_frames = config_.freewheel_period_frames,
                    .driver = driver,
                    .record_frames = 0,
                    .realtime_priority = true,
                    .start_time = start_time,
                    .device = nullptr,
                });
    if (!opened) {
        return; // Only an invalid OutputConfig gets here.
    }
    core_->activate(driver);
    if ((*opened)->start()) {
        freewheel_ = std::move(*opened);
    }
}

void OutputController::close(std::unique_ptr<AudioBackend>& backend) noexcept {
    if (backend) {
        backend->stop();
        backend.reset();
    }
}

} // namespace ks::engine
