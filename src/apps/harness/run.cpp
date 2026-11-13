// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/harness/run.h>

#include <ks/harness/script.h>
#include <ks/harness/tracks.h>

#include <atomic>
#include <expected>
#include <ostream>
#include <string>

namespace ks::harness {

// Stub (specs/harness/spec.md requirement 28).
bool flags_problem(const Report& /*report*/) noexcept {
    return false;
}

// Stub (specs/harness/spec.md requirement 28).
void print_report(const Report& /*report*/, std::ostream& /*out*/) {}

// Stub (specs/harness/spec.md requirements 17 to 20 and 24).
std::expected<Report, std::string> render_to_file(const Script& /*script*/,
                                                  const TrackSet& /*tracks*/,
                                                  const FileOptions& /*options*/,
                                                  const std::atomic<bool>& /*stop*/) {
    return std::unexpected{std::string{"not implemented"}};
}

// Stub (specs/harness/spec.md requirements 17 to 20 and 25 to 27).
std::expected<Report, std::string> play_on_device(const Script& /*script*/,
                                                  const TrackSet& /*tracks*/,
                                                  const DeviceOptions& /*options*/,
                                                  const std::atomic<bool>& /*stop*/,
                                                  std::ostream& /*out*/) {
    return std::unexpected{std::string{"not implemented"}};
}

} // namespace ks::harness
