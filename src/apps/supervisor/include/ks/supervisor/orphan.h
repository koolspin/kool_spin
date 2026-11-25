// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// An engine that outlived its supervisor: finding it, stopping it, and asking
/// the user whether to (specs/supervisor-crash/spec.md requirements 2 to 4 and
/// 7). It isn't this process's child, so its lock file is all there is to go
/// by: held means that it runs, and the process ID in the file says where.

#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace ks::supervisor {

/// How far apart two probes must read the same process ID to believe it: an
/// engine takes its lock just before it writes its ID.
inline constexpr std::chrono::milliseconds kOrphanSettle{100};
/// How long find_orphan() goes on looking for such a pair.
inline constexpr std::chrono::milliseconds kOrphanSettleLimit{1'000};
/// How often stop_orphan() looks whether the lock is free.
inline constexpr std::chrono::milliseconds kOrphanPoll{10};
/// How long an orphan has to end after SIGKILL.
inline constexpr std::chrono::milliseconds kOrphanKillTimeout{1'000};

/// Waits for a while: std::this_thread::sleep_for(), except in tests.
/// find_orphan()'s limit counts what it asks this to wait; stop_orphan()'s
/// limits are by the clock.
using Wait = std::function<void(std::chrono::milliseconds)>;

/// The process that holds the engine lock.
struct Orphan {
    /// Its process ID, if two probes in a row read the same valid one, and it
    /// isn't this process's.
    std::optional<pid_t> pid;
};

/// Looks for an orphan in the runtime directory `dir`: nothing if the engine
/// lock is free, or comes free meanwhile. The error is a probe's.
[[nodiscard]] std::expected<std::optional<Orphan>, std::error_code>
find_orphan(const std::filesystem::path& dir, const Wait& wait);

/// How stop_orphan() ended.
enum class Stopped : std::uint8_t {
    Gone,   ///< It had ended before it was sent anything.
    Faded,  ///< It ended after SIGTERM, which fades an engine out.
    Killed, ///< It didn't, and ended after SIGKILL.
};

/// Stops the orphan find_orphan() found: SIGTERM, up to kEngineStopTimeout,
/// SIGKILL, and up to kOrphanKillTimeout. Before each signal, it checks that
/// the lock is still held, and that the file still names `pid`; if not, it
/// signals nothing. Says why it couldn't stop it.
[[nodiscard]] std::expected<Stopped, std::string> stop_orphan(const std::filesystem::path& dir,
                                                              pid_t pid, const Wait& wait);

/// The longest answer read_answer() takes as one.
inline constexpr std::size_t kMaxAnswer = 64;

/// True if there's someone to ask: `in` and `err` are terminals, and this
/// process's group is the one in the foreground of `in`, so that reading it
/// doesn't stop the process.
[[nodiscard]] bool can_ask(int in, int err) noexcept;

/// True if `answer`, a line without its newline, is "y" or "yes", in any case,
/// with blanks around it ignored.
[[nodiscard]] bool is_yes(std::string_view answer) noexcept;

/// Reads a line from `in`, all of it, so that nothing is left for the shell.
/// True if it is_yes(). A line longer than kMaxAnswer, end of file, and a read
/// error are all no.
[[nodiscard]] bool read_answer(int in) noexcept;

} // namespace ks::supervisor
