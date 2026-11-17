// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The decode worker process: one job, from the supervisor's Probe to its
/// Decoded reply (specs/decode-worker/spec.md requirements 17 to 20).

#include <ks/worker_app/options.h>

#include <chrono>
#include <ostream>

namespace ks::worker_app {

/// The longest the worker goes, while it waits, between reading the
/// supervisor's socket and checking for a stop signal (requirement 19).
inline constexpr std::chrono::milliseconds kWaitInterval{10};

/// Does what `options` asks: prints the usage for --help, and otherwise runs
/// the job. Blocks SIGTERM, SIGINT, and SIGHUP and installs no handlers, so
/// call it once, from main(). Writes the usage to `out`, and reports to
/// `err`. Returns the exit status: kExitOk once the job has ended, or
/// kExitStartFailed if it couldn't start.
[[nodiscard]] int run(const Options& options, std::ostream& out, std::ostream& err);

} // namespace ks::worker_app
