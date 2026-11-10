// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Rules shared by every layout that lives in shared memory (ADR 0003 §7).
///
/// Shared layouts hold only fixed-width integers and arrays of them. Words that
/// two processes access at the same time are plain std::uint64_t, read and
/// written only through std::atomic_ref. Plain integers keep the layouts
/// trivially copyable, so zero-filled memory is a valid value and nothing has
/// to be constructed in shared memory.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ks::ipc {

/// Minimum distance between fields that different processes write, so they
/// never share a cache line. Apple Silicon's cache lines are 128 bytes, and
/// x86 fetches 64-byte lines in adjacent pairs, so 128 is safe on both.
inline constexpr std::size_t kSharedAlignment = 128;

static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free,
              "shared memory needs lock-free 64-bit atomics");

} // namespace ks::ipc
