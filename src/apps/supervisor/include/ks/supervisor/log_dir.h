// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ks/ipc/log.h>

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

/// The log folder and the session folders in it (specs/logs/spec.md,
/// requirements 9 to 11).
namespace ks::supervisor {

/// How many session folders the log folder holds, the new one included.
inline constexpr std::size_t kSessionsKept = 10;

/// The default log folder, from the values of HOME and XDG_STATE_HOME, each
/// empty-handed if the variable isn't set (requirement 9):
/// - Linux: `<xdg_state_home>/kool_spin/logs` if that's an absolute path, and
///   `<home>/.local/state/kool_spin/logs` otherwise;
/// - macOS: `<home>/Library/Logs/kool_spin`.
///
/// Nothing if it needs `home` and `home` isn't an absolute path.
[[nodiscard]] std::optional<std::filesystem::path>
default_log_dir(std::optional<std::string_view> home,
                std::optional<std::string_view> xdg_state_home);

/// The default log folder, from the environment.
[[nodiscard]] std::optional<std::filesystem::path> default_log_dir();

/// Creates the folder `path` and any missing parents, each with mode 0700.
/// A folder that's already there is left as it is.
[[nodiscard]] std::error_code create_log_dir(const std::filesystem::path& path);

/// A session folder's name, `YYYY-MM-DD_HH-MM-SS_<pid>`: the clock's system
/// time, in its local time (requirement 10). Nothing if that time falls
/// outside the years 1970 to 9999, or `pid` is negative.
[[nodiscard]] std::optional<std::string> session_name(const ipc::LogClock& clock, pid_t pid);

/// The parts of a session folder's name.
struct SessionName {
    std::string_view time; ///< `YYYY-MM-DD_HH-MM-SS`: these sort by time.
    std::uint64_t pid = 0;
};

/// The parts of `name`, if it has the session pattern: digits where
/// `YYYY-MM-DD_HH-MM-SS_<pid>` has letters, with a process ID of 1 to 19
/// digits. The result's `time` points into `name`.
[[nodiscard]] std::optional<SessionName> parse_session_name(std::string_view name) noexcept;

/// Keeps the `keep` newest folders in `log_dir` whose names have the session
/// pattern, ordered by the time in their names, then by process ID, and
/// removes the others (requirement 11). Removing one means deleting the
/// regular files in it whose names end in `.log`, then the folder itself if
/// that left it empty. Symbolic links are never followed, and nothing else is
/// deleted. Whatever can't be read or removed is left where it is.
void remove_old_sessions(const std::filesystem::path& log_dir,
                         std::size_t keep = kSessionsKept - 1);

} // namespace ks::supervisor
