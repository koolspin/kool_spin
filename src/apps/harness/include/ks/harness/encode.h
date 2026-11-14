// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Encoding commands into command slots, the inverse of engine::decode()
/// (specs/harness/spec.md requirement 17).

#include <ks/engine/command.h>
#include <ks/ipc/command.h>

#include <cstdint>

namespace ks::harness {

/// The slot for `command`, numbered `sequence`, as specs/commands/spec.md
/// requirement 2 lays it out. For a command whose unused fields hold their
/// defaults, as decode() returns them, decode() of the slot gives back
/// `command`.
[[nodiscard]] ipc::CommandSlot encode(const engine::Command& command,
                                      std::uint64_t sequence) noexcept;

} // namespace ks::harness
