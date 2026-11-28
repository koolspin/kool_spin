// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// `ks-play status`'s lines (specs/ks-play/spec.md requirement 25). Private
/// to ks_play_app; its unit tests include it directly.

#include <ks/client/snapshot.h>

#include <ostream>

namespace ks::play_app {

/// Writes `snapshot` as requirement 25's lines: `engine`, `deck 0` to
/// `deck 3`, `channel 0` to `channel 3`, `mixer`, then a `client <n>` line
/// for each slot that's attached or corrupt. Reals use `%.6g`, flags `0` or
/// `1`, and a rejection's name has its spaces written `_`.
void format_status(const client::Snapshot& snapshot, std::ostream& out);

/// Writes `health` as specs/health-checks/spec.md requirement 19's `health`
/// line.
void format_health(const client::HealthInfo& health, std::ostream& out);

} // namespace ks::play_app
