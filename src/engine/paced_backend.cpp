// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/paced_backend.h>

namespace ks::engine {

// Stubs: the acceptance tests compile against this API before it's
// implemented (specs/device-loss/spec.md). The stubs make every test fail.

void SimulatedDevice::unplug(Loss /*loss*/) noexcept {}

void SimulatedDevice::plug_in() noexcept {}

bool SimulatedDevice::present() const noexcept KS_NONBLOCKING {
    static_cast<void>(present_);
    return true;
}

std::uint64_t SimulatedDevice::unplugs() const noexcept KS_NONBLOCKING {
    static_cast<void>(unplugs_);
    return 0;
}

Loss SimulatedDevice::last_loss() const noexcept KS_NONBLOCKING {
    static_cast<void>(last_loss_);
    return Loss::Silent;
}

std::expected<std::unique_ptr<PacedBackend>, BackendError>
open_paced(RenderCore& /*core*/, const PacedConfig& /*config*/) {
    return std::unexpected{
        BackendError{.kind = BackendError::Kind::NoBackend, .message = "not implemented yet"}};
}

PacedDeviceSource::PacedDeviceSource(SimulatedDevice& device, const PacedConfig& config) noexcept
    : device_{&device}, config_{config} {}

bool PacedDeviceSource::available() {
    static_cast<void>(device_);
    return false;
}

std::expected<std::unique_ptr<AudioBackend>, BackendError>
PacedDeviceSource::open(RenderCore& /*core*/, DriverId /*driver*/) {
    static_cast<void>(config_);
    return std::unexpected{
        BackendError{.kind = BackendError::Kind::NoBackend, .message = "not implemented yet"}};
}

} // namespace ks::engine
