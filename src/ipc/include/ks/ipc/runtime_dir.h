// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The per-user runtime directory, and lock files that show which processes
/// are alive (ADR 0003 §6 and §12, specs/segments/spec.md requirements 18
/// to 23).

#include <ks/ipc/fd.h>

#include <sys/types.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace ks::ipc {

/// The runtime directory for `base`: `$XDG_RUNTIME_DIR` on Linux, `$TMPDIR`
/// on macOS, or nothing if the variable isn't set.
/// - `base` set, non-empty, and absolute: `<base>/kool_spin`.
/// - Otherwise, Linux: `/tmp/kool_spin-<uid>`, with the effective user ID.
/// - Otherwise, macOS: `kool_spin` in the per-user temporary directory from
///   confstr(_CS_DARWIN_USER_TEMP_DIR), or `/tmp/kool_spin-<uid>` if that
///   fails.
[[nodiscard]] std::filesystem::path runtime_directory_path(std::optional<std::string_view> base);

/// runtime_directory_path() for this process's environment.
[[nodiscard]] std::filesystem::path default_runtime_directory_path();

/// Creates the directory `path` with mode 0700 if it doesn't exist, then
/// checks it. Without changing it, refuses a directory that isn't:
/// - a directory, and not a symbolic link (std::errc::not_a_directory);
/// - owned by the effective user (std::errc::permission_denied);
/// - inaccessible to the group and to others (std::errc::permission_denied).
[[nodiscard]] std::expected<void, std::error_code>
open_runtime_directory(const std::filesystem::path& path);

/// The longest lock-file name.
inline constexpr std::size_t kMaxLockNameLength = 64;

/// Whether a lock file is held, and by which process.
struct LockState {
    bool held = false;
    /// The holder's process ID, if its file holds a valid one. Trust it only
    /// while the lock is held: the holder is alive, so its ID can't have been
    /// reused.
    std::optional<pid_t> pid;
};

/// An exclusive flock() on `<dir>/<name>.lock`, held until the LockFile is
/// destroyed or the process dies. The file holds the holder's process ID.
/// Move-only. A process created with fork() shares its parent's lock.
class LockFile {
public:
    /// Why acquire() failed.
    struct Failure {
        /// std::errc::resource_unavailable_try_again when another process
        /// holds the lock; std::errc::invalid_argument for an invalid name;
        /// otherwise the system call's error.
        std::error_code error;
        /// The holder's process ID, when another process holds the lock and
        /// its file holds a valid one.
        std::optional<pid_t> holder;
    };

    /// Opens or creates `<dir>/<name>.lock` with mode 0600, refusing a
    /// symbolic link, takes an exclusive flock() without waiting, and
    /// replaces the file's contents with this process's ID in decimal and a
    /// newline.
    ///
    /// A name is 1 to kMaxLockNameLength characters from a-z, 0-9, '-', and
    /// '_'.
    [[nodiscard]] static std::expected<LockFile, Failure> acquire(const std::filesystem::path& dir,
                                                                  std::string_view name);

    LockFile(const LockFile&) = delete;
    LockFile& operator=(const LockFile&) = delete;
    LockFile(LockFile&&) noexcept = default;
    LockFile& operator=(LockFile&&) noexcept = default;
    /// Releases the lock. The file stays.
    ~LockFile() = default;

private:
    explicit LockFile(Fd fd) noexcept : fd_{std::move(fd)} {}

    Fd fd_;
};

/// Reports, without waiting, whether `<dir>/<name>.lock` is held:
/// - free if there's no such file, or nobody holds its lock;
/// - held, with the holder's ID if the file holds a valid one: a positive
///   decimal number that fits pid_t, followed by a newline or nothing.
///
/// An acquire() at exactly the same moment can fail because of the probe.
/// Refuses an invalid name with std::errc::invalid_argument.
[[nodiscard]] std::expected<LockState, std::error_code> probe_lock(const std::filesystem::path& dir,
                                                                   std::string_view name);

} // namespace ks::ipc
