// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/output_controller.h>

namespace ks::engine {

// Stubs: the acceptance tests compile against this API before it's
// implemented (specs/device-loss/spec.md). The stubs make every test fail.

OutputController::OutputController(RenderCore& core, DeviceSource& source,
                                   const OutputConfig& config) noexcept
    : core_{&core}, source_{&source}, config_{config} {}

OutputController::~OutputController() = default;

void OutputController::start() {
    static_cast<void>(core_);
    static_cast<void>(source_);
    static_cast<void>(config_);
    static_cast<void>(device_driver_);
    static_cast<void>(next_driver_);
    static_cast<void>(sample_rate_);
    static_cast<void>(channels_);
    static_cast<void>(device_started_);
    static_cast<void>(last_progress_);
    static_cast<void>(last_frames_);
    static_cast<void>(last_retry_);
}

void OutputController::poll() {}

void OutputController::stop() {}

OutputState OutputController::state() const noexcept {
    return state_;
}

} // namespace ks::engine
