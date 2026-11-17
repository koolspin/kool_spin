// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// ks-worker's command line and exit statuses (specs/decode-worker/spec.md
/// requirement 16).

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ks::worker_app {

/// The exit statuses (requirements 16 to 19).
inline constexpr int kExitOk = 0;
/// It couldn't start.
inline constexpr int kExitStartFailed = 1;
inline constexpr int kExitUsageError = 2;

/// ks-worker's command line, parsed. Options not given are empty.
struct Options {
    /// Its end of the socket pair to the supervisor.
    std::optional<int> supervisor_fd;
    /// The log segment, if the supervisor gives one.
    std::optional<int> log_fd;
    bool help = false;
};

/// The usage text that --help and usage errors print. Its first line is
/// "Usage: ks-worker --supervisor-fd <n> [--log-fd <n>]".
[[nodiscard]] std::string usage();

/// Parses ks-worker's arguments, not including argv[0]. Parsing stops at
/// --help, which sets `help`. A missing --supervisor-fd, or an unknown or
/// repeated option, is a usage error: returns a one-line message starting
/// "ks-worker: ".
[[nodiscard]] std::expected<Options, std::string>
parse_options(std::span<const std::string_view> args);

} // namespace ks::worker_app
