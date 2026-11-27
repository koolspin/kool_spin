// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/engine_app/health.h>

// Step 10a: not built yet. These stubs let the acceptance tests build.

namespace ks::engine_app {

PowerProfileReading read_power_profile(const std::filesystem::path& /*root*/) {
    return {};
}

std::uint64_t read_power_source(const std::filesystem::path& /*root*/) {
    return ipc::kPowerSourceUnknown;
}

std::vector<engine::EventLine> HealthMonitor::pass(Clock::time_point /*now*/,
                                                   const HealthReadings& /*readings*/) {
    loads_.clear();
    return {};
}

} // namespace ks::engine_app
