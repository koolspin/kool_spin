// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// ks-play's command line (specs/ks-play/spec.md requirements 21 and 22),
/// parsed without touching the network, the file system, or the
/// environment, so it can be fuzzed.

#include <ks/client/command.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace ks::play_app {

/// `load <deck> <path> [--wait]`.
struct Load {
    std::uint32_t deck = 0;
    /// Absolute: the argument resolved against the working directory,
    /// lexically, without following symbolic links.
    std::string path;
    bool wait = false;
};

/// `unload <deck>`.
struct Unload {
    std::uint32_t deck = 0;
};

/// Every command that goes through a ring.
struct Control {
    client::Command command;
};

/// `status [--watch] [--interval <ms>]`.
struct Status {
    bool watch = false;
    /// 50 to 60,000.
    std::uint32_t interval_ms = 500;
};

struct Stop {};
struct Ping {};
/// `--help`.
struct Help {};

using Request = std::variant<Load, Unload, Control, Status, Stop, Ping, Help>;

/// A parsed command line.
struct Invocation {
    std::filesystem::path runtime_dir;
    /// The Hello name: 1 to 64 bytes.
    std::string name = "ks-play";
    Request request;
};

/// Parses `args` (without the program name). `cwd` resolves a relative path
/// for `load`, and `default_runtime_dir` is the runtime directory when the
/// command line doesn't give one. Returns why not for a usage error: the
/// text is one line, without the `ks-play: ` prefix.
[[nodiscard]] std::expected<Invocation, std::string>
parse(std::span<const std::string_view> args, const std::filesystem::path& cwd,
      const std::filesystem::path& default_runtime_dir);

/// The usage text for --help.
[[nodiscard]] std::string usage();

} // namespace ks::play_app
