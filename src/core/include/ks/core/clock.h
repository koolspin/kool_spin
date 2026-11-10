// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Reading the monotonic clock on the audio thread.

#include <ks/core/realtime.h>

#include <chrono>

namespace ks::core {

/// `std::chrono::steady_clock::now()`, callable from KS_NONBLOCKING code.
[[nodiscard]] inline std::chrono::steady_clock::time_point monotonic_now() noexcept KS_NONBLOCKING {
    // Stub: the acceptance tests compile against this API before it's
    // implemented (specs/render-core/spec.md).
    return {};
}

} // namespace ks::core
