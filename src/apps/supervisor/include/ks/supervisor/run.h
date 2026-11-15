// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The supervisor: it starts the engine, welcomes clients, and stops
/// everything with a fade-out (specs/processes/spec.md requirements 15 to 22).

#include <ks/supervisor/options.h>

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <ostream>
#include <system_error>

namespace ks::supervisor {

/// How long the engine has to send Ready after it starts (requirement 15).
inline constexpr std::chrono::seconds kEngineReadyTimeout{10};
/// How long the engine has to exit after it's asked to stop (requirements 19
/// and 21).
inline constexpr std::chrono::seconds kEngineStopTimeout{2};
/// How long the supervisor keeps sending what's queued for clients after the
/// engine has stopped (requirement 19).
inline constexpr std::chrono::seconds kClientFlushTimeout{1};
/// The most client connections open at once (requirement 18).
inline constexpr std::size_t kMaxConnections = 16;

/// `ks-engine` in the directory that holds this process's executable: the
/// default for --engine (requirement 13).
[[nodiscard]] std::expected<std::filesystem::path, std::error_code> default_engine_path();

/// Does what `options` asks: prints the usage for --help, and otherwise runs
/// the supervisor until it stops. Installs handlers for SIGCHLD, SIGINT,
/// SIGTERM, and SIGHUP, so call it once, from main(). Writes the usage to
/// `out`, and reports to `err`. Returns the exit status (requirement 14).
[[nodiscard]] int run(const Options& options, std::ostream& out, std::ostream& err);

} // namespace ks::supervisor
