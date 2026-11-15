// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Starting child processes (specs/processes/spec.md requirement 17; ADR 0003
/// §13).

#include <sys/types.h>

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

namespace ks::supervisor {

/// A descriptor a child inherits: the parent's `parent`, as `child` in the
/// child.
struct Inherit {
    int parent = -1;
    int child = -1;
};

/// Starts `program` with posix_spawn, with `args` as its arguments after
/// argv[0], which is `program` itself, and returns its process ID. The child:
/// - leads a new process group;
/// - starts with an empty signal mask, and every signal at its default
///   disposition;
/// - inherits descriptors 0, 1, and 2, and each of `fds` at its `child`
///   number, and no other descriptor, even one this process inherited without
///   close-on-exec;
/// - doesn't ask to be killed when its parent dies.
///
/// Fails with the error if the program can't be started, for example because
/// it doesn't exist or isn't executable. Refuses, with
/// std::errc::invalid_argument, a `child` number below 3 or given twice, and
/// a negative `parent`.
[[nodiscard]] std::expected<pid_t, std::error_code> spawn(const std::filesystem::path& program,
                                                          std::span<const std::string> args,
                                                          std::span<const Inherit> fds);

} // namespace ks::supervisor
