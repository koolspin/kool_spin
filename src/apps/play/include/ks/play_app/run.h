// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// ks-play's work: connecting, doing one command, and reporting
/// (specs/ks-play/spec.md requirements 23 to 29).

#include <ks/play_app/options.h>

#include <iosfwd>

namespace ks::play_app {

/// Exit statuses (requirement 27). A signal ends ks-play with 130 for SIGINT
/// and 143 for SIGTERM, except in `status --watch` (requirement 29).
inline constexpr int kExitOk = 0;
inline constexpr int kExitFailed = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitUnreachable = 3;

/// Runs `invocation`: results to `out`, everything else to `err`. Handles
/// SIGINT and SIGTERM, and ignores SIGPIPE, while it runs. Returns the exit
/// status.
[[nodiscard]] int run(const Invocation& invocation, std::ostream& out, std::ostream& err);

/// The whole program: parses `argc` and `argv`, reports usage errors to
/// `err` (requirement 27), and runs.
[[nodiscard]] int main_entry(int argc, const char* const* argv, std::ostream& out,
                             std::ostream& err);

} // namespace ks::play_app
