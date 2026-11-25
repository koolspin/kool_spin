// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The per-user state folder: permanent, unlike the runtime directory, so a
/// lock in it finds an engine from an earlier session whatever happened to
/// the runtime directory (ADR 0003 §12 and §14, specs/state-folder/spec.md
/// requirements 1 to 4).

#ifdef __APPLE__
#include <sys/mount.h>
#include <sys/param.h>
#else
#include <sys/vfs.h>
#endif

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace ks::ipc {

/// The state folder for these values, each of which may be missing; one that
/// isn't an absolute path counts as missing.
/// - Linux: `<xdg_state_home>/kool_spin`, or else
///   `<home>/.local/state/kool_spin`, or else the same under `account_home`.
/// - macOS: `<home>/Library/Application Support/kool_spin`, or else the same
///   under `account_home`.
///
/// Nothing if all that it could use are missing.
[[nodiscard]] std::optional<std::filesystem::path>
state_directory_path(std::optional<std::string_view> home,
                     std::optional<std::string_view> xdg_state_home,
                     std::optional<std::string_view> account_home);

/// state_directory_path() for `HOME`, `XDG_STATE_HOME`, and the effective
/// user's home directory in the user database, which it reads only if it's
/// needed.
[[nodiscard]] std::optional<std::filesystem::path> default_state_directory_path();

/// Where a process's state folder comes from (requirement 2).
enum class StateDirectorySource : std::uint8_t {
    None,    ///< It uses none: `--runtime-dir` without `--state-dir`.
    Default, ///< default_state_directory_path().
    Named,   ///< `--state-dir` named it.
};

/// The state folder a process should use.
struct StateDirectoryChoice {
    StateDirectorySource source = StateDirectorySource::None;
    /// Nothing for None, and for a Default that doesn't exist: there's no
    /// path.
    std::optional<std::filesystem::path> path;
};

/// The state folder for a command line: `state_dir` is `--state-dir`'s value,
/// and `has_runtime_dir` says whether `--runtime-dir` was given.
[[nodiscard]] StateDirectoryChoice
choose_state_directory(const std::optional<std::filesystem::path>& state_dir, bool has_runtime_dir);

/// Whether the state folder may be on a file system (requirement 4).
enum class FileSystemVerdict : std::uint8_t {
    Usable,
    /// Refused: a network file system.
    Network,
    /// Refused: FUSE, which may be one, and `--state-dir` didn't name the
    /// folder. Linux only.
    Fuse,
};

/// Judges the file system that `statfs` describes. `named` says whether
/// `--state-dir` named the folder.
[[nodiscard]] FileSystemVerdict judge_file_system(const struct ::statfs& status,
                                                  bool named) noexcept;

/// Why check_state_directory() or open_state_directory() failed.
struct StateDirectoryFailure {
    /// std::errc::not_supported for a file system judge_file_system()
    /// refuses; std::errc::not_a_directory, and std::errc::permission_denied
    /// for one that another user owns or that others can write; otherwise the
    /// system call's error.
    std::error_code error;
    /// True if it was refused only because `--state-dir` didn't name it.
    bool fuse = false;
    /// True if it was refused because the group or others can write it.
    bool writable = false;
};

/// What went wrong, for a warning about the folder, with what would put it
/// right if the user can.
[[nodiscard]] std::string describe(const StateDirectoryFailure& failure);

/// Checks the folder `path` without creating or changing anything. Refuses one
/// that isn't a directory, or a symbolic link to one; that the effective user
/// doesn't own; that the group or others can write; or that
/// judge_file_system() refuses.
[[nodiscard]] std::expected<void, StateDirectoryFailure>
check_state_directory(const std::filesystem::path& path, bool named);

/// Creates the folder `path` and any missing parents, each with mode 0700,
/// then does check_state_directory(). A folder that's there isn't changed.
[[nodiscard]] std::expected<void, StateDirectoryFailure>
open_state_directory(const std::filesystem::path& path, bool named);

} // namespace ks::ipc
