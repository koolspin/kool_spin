// SPDX-License-Identifier: GPL-3.0-or-later
// ks-harness: drives the engine from a script, through a device or into a
// WAV file (specs/harness/spec.md). Everything but argument handling is in
// the ks::harness library.

#include <ks/harness/options.h>

#include <atomic>
#include <csignal>
#include <expected>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// Set on SIGINT, so a run stops early and still prints its report
/// (requirement 26). A signal handler can reach only a global.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): see above
std::atomic<bool> stop_requested{false};

static_assert(std::atomic<bool>::is_always_lock_free, "a signal handler may store it");

extern "C" void on_interrupt(int /*signal*/) {
    stop_requested.store(true, std::memory_order_relaxed);
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> arguments{argv, static_cast<std::size_t>(argc)};
    std::vector<std::string_view> args;
    for (const char* argument : arguments.subspan(1)) {
        args.emplace_back(argument);
    }

    const std::expected<ks::harness::Options, std::string> options =
        ks::harness::parse_options(args);
    if (!options) {
        std::cerr << options.error() << "\n\n" << ks::harness::usage();
        return ks::harness::kExitUsageError;
    }

    static_cast<void>(std::signal(SIGINT, &on_interrupt));
    return ks::harness::run(*options, std::cout, std::cerr, stop_requested);
}
