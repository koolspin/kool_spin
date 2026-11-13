// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/harness/run.h>

#include <ks/harness/script.h>
#include <ks/harness/tracks.h>

#include <atomic>
#include <expected>
#include <format>
#include <ostream>
#include <string>

namespace ks::harness {

bool flags_problem(const Report& report) noexcept {
    return report.late_blocks > 0 || report.non_finite_samples > 0 ||
           report.commands_rejected > 0 || report.commands_read < report.commands_sent;
}

void print_report(const Report& report, std::ostream& out) {
    out << std::format("Played {:.3f} s\n", report.played_seconds);
    if (report.stopped_early) {
        out << "Stopped early\n";
    }
    out << std::format("Blocks: {} rendered, {} late, peak load {:.3f}\n", report.blocks_rendered,
                       report.late_blocks, report.peak_render_load);
    out << std::format("Samples: {} non-finite, {} limited\n", report.non_finite_samples,
                       report.limited_frames);
    out << std::format("Commands: {} sent, {} read, {} rejected, {} delayed\n",
                       report.commands_sent, report.commands_read, report.commands_rejected,
                       report.events_delayed);
    out << std::format("Device losses: {}\n", report.device_losses);
}

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
