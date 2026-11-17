// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// What a process the supervisor started does with what it inherits: its
/// descriptors, and its stop signals. ks-engine and ks-worker install no
/// signal handlers: they block their stop signals before any thread starts,
/// so every thread inherits the mask, and take them on their main thread
/// (specs/audio-events/spec.md requirement 9; specs/decode-worker/spec.md
/// requirement 17).

#include <array>
#include <csignal>
#include <system_error>

namespace ks::ipc {

/// The signals that stop a child process.
inline constexpr std::array<int, 3> kStopSignals{SIGTERM, SIGINT, SIGHUP};

/// Blocks kStopSignals in the calling thread, and so in every thread it
/// creates from then on. Returns the error, if any. The mask stays until the
/// process exits: unblocked, a second signal sent while it stops would end it
/// by its default action.
[[nodiscard]] std::error_code block_stop_signals() noexcept;

/// Takes a pending stop signal, if there is one. True if it took one.
/// @pre block_stop_signals() has blocked them in every thread.
[[nodiscard]] bool take_stop_signal() noexcept;

/// Takes on an inherited socket: true if `fd` is an open stream socket, made
/// close-on-exec and non-blocking.
[[nodiscard]] bool adopt_socket(int fd) noexcept;

/// Takes on an inherited descriptor: true if `fd` is open, made
/// close-on-exec.
[[nodiscard]] bool adopt_descriptor(int fd) noexcept;

} // namespace ks::ipc
