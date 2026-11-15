// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The engine process: it takes its descriptors from the supervisor, plays
/// until it's stopped, and fades out (specs/processes/spec.md requirements 9
/// to 12).

#include <ks/engine_app/options.h>

#include <chrono>
#include <ostream>

namespace ks::engine_app {

/// The longest the control thread goes between reading the supervisor's
/// socket and polling the output controller (requirement 10).
inline constexpr std::chrono::milliseconds kControlInterval{10};

/// Does what `options` asks: prints the usage for --help, and otherwise
/// starts the engine and runs its control thread until it's stopped.
/// Installs handlers for SIGTERM, SIGINT, and SIGHUP, so call it once, from
/// main(). Writes the usage to `out`, and reports to `err`. Returns the exit
/// status: kExitOk once stopped, kExitStartFailed if it couldn't start, or
/// kExitUsageError for a device number that isn't listed.
[[nodiscard]] int run(const Options& options, std::ostream& out, std::ostream& err);

} // namespace ks::engine_app
