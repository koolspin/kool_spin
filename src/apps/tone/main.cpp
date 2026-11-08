// SPDX-License-Identifier: GPL-3.0-or-later
//
// ks-tone: plays a test tone on the default output device.

#include <ks/tone/tone_player.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <iostream>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace {

constexpr auto kDuration = std::chrono::seconds{2};

void print_usage(std::ostream& out) {
    std::string backends;
    for (const std::string_view name : ks::tone::backend_names()) {
        backends += backends.empty() ? "" : ", ";
        backends += name;
    }
    const ks::tone::ToneSettings settings;
    out << std::format("Usage: ks-tone [--backend NAME]\n"
                       "\n"
                       "Plays a {:g} Hz test tone at {:g} dBFS for {} s on the default output "
                       "device.\n"
                       "\n"
                       "  --backend NAME  audio backend: {}\n"
                       "                  (default: the first one that works)\n"
                       "  -h, --help      show this help\n",
                       settings.frequency_hz, settings.level_dbfs, kDuration.count(), backends);
}

void print_device(const ks::tone::DeviceInfo& info) {
    const double buffer_ms =
        1000.0 * info.period_frames * info.periods / static_cast<double>(info.sample_rate);
    std::cout << std::format("Playing on {}: {}\n"
                             "  {} Hz, {} channels, {} frames x {} periods ({:.1f} ms)\n",
                             info.backend, info.device, info.sample_rate, info.channels,
                             info.period_frames, info.periods, buffer_ms)
              << std::flush;
}

} // namespace

int main(int argc, char* argv[]) {
    const std::span args{argv, static_cast<std::size_t>(argc)};
    std::string_view backend;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(std::cout);
            return 0;
        }
        if (arg == "--backend" && i + 1 < args.size()) {
            backend = args[++i];
            continue;
        }
        std::cerr << std::format("ks-tone: unexpected argument '{}'\n\n", arg);
        print_usage(std::cerr);
        return 2;
    }

    const auto report =
        ks::tone::play_tone(backend, ks::tone::ToneSettings{}, kDuration, print_device);
    if (!report) {
        std::cerr << std::format("ks-tone: {}\n", report.error());
        return 1;
    }
    std::cout << std::format("Done: {} frames, peak {:.1f} dBFS\n", report->frames_rendered,
                             20.0 * std::log10(static_cast<double>(report->peak)));
    return 0;
}
