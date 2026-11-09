// SPDX-License-Identifier: GPL-3.0-or-later
//
// ks-tone: plays a test tone on an output device.

#include <ks/tone/options.h>
#include <ks/tone/tone_player.h>

#include <cmath>
#include <cstddef>
#include <format>
#include <iostream>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_usage(std::ostream& out) {
    using namespace ks::tone;
    std::string backends;
    for (const std::string_view name : backend_names()) {
        backends += backends.empty() ? "" : ", ";
        backends += name;
    }
    out << std::format(
        "Usage: ks-tone [options]\n"
        "\n"
        "Plays a test tone, then prints the peak level and frequency actually played.\n"
        "\n"
        "  --frequency HZ      tone frequency, {} to {} Hz (default: {})\n"
        "  --level DBFS        peak level, {} to {} dBFS (default: {})\n"
        "  --duration SECONDS  how long to play, {} to {} s (default: {})\n"
        "  --backend NAME      audio backend: {}\n"
        "                      (default: the first one that works)\n"
        "  --list-devices      list the backend's output devices and exit\n"
        "  --device N          play on device N from --list-devices\n"
        "                      (default: the system default device)\n"
        "  -h, --help          show this help\n",
        kMinFrequencyHz, kMaxFrequencyHz, kDefaultFrequencyHz, kMinLevelDbfs, kMaxLevelDbfs,
        kDefaultLevelDbfs, kMinDurationSeconds, kMaxDurationSeconds, kDefaultDurationSeconds,
        backends);
}

void print_start(const ks::tone::Options& options, const ks::tone::DeviceInfo& info) {
    const double buffer_ms =
        1000.0 * info.period_frames * info.periods / static_cast<double>(info.sample_rate);
    std::cout << std::format("Playing on {}: {}\n"
                             "  {} Hz, {} channels, {} frames x {} periods ({:.1f} ms)\n"
                             "  {} Hz at {} dBFS for {} s\n",
                             info.backend, info.device, info.sample_rate, info.channels,
                             info.period_frames, info.periods, buffer_ms,
                             options.settings.frequency_hz, options.settings.level_dbfs,
                             options.duration.count())
              << std::flush;
}

void print_devices(const ks::tone::DeviceList& list) {
    std::cout << std::format("Output devices on {}:\n", list.backend);
    for (std::size_t i = 0; i < list.devices.size(); ++i) {
        const auto& device = list.devices[i];
        std::cout << std::format("  {}: {}{}\n", i, device.name,
                                 device.is_default ? " (default)" : "");
    }
}

} // namespace

int main(int argc, char* argv[]) {
    const std::span argv_span{argv, static_cast<std::size_t>(argc)};
    const auto arguments = argv_span.subspan(argv_span.empty() ? 0 : 1); // skip argv[0]
    const std::vector<std::string_view> args(arguments.begin(), arguments.end());

    const auto options = ks::tone::parse_options(args);
    if (!options) {
        std::cerr << options.error() << '\n';
        return 2;
    }
    if (options->help) {
        print_usage(std::cout);
        return 0;
    }
    if (options->list_devices) {
        const auto list = ks::tone::list_devices(options->backend);
        if (!list) {
            std::cerr << std::format("ks-tone: {}\n", list.error());
            return 1;
        }
        print_devices(*list);
        return 0;
    }
    // --device is parsed but not acted on yet.

    const auto report = ks::tone::play_tone(
        options->backend, options->settings, options->duration,
        [&options](const ks::tone::DeviceInfo& info) { print_start(*options, info); });
    if (!report) {
        std::cerr << std::format("ks-tone: {}\n", report.error());
        return 1;
    }
    std::cout << std::format("Done: {} frames, peak {:.1f} dBFS, {} Hz\n", report->frames_rendered,
                             20.0 * std::log10(static_cast<double>(report->peak)),
                             std::lround(report->measured_hz));
    return 0;
}
