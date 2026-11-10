// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Reading the monotonic clock on the audio thread.

#include <ks/core/realtime.h>

#include <chrono>

namespace ks::core {

/// `std::chrono::steady_clock::now()`, callable from KS_NONBLOCKING code.
[[nodiscard]] inline std::chrono::steady_clock::time_point monotonic_now() noexcept KS_NONBLOCKING {
    // steady_clock::now() isn't annotated, so function effect analysis rejects
    // the call. It doesn't block: it reads the clock, usually without even a
    // system call (the vDSO on Linux, the commpage on macOS). RealtimeSanitizer
    // still checks it at run time. Apart from the guardrail canaries, this is
    // the only function-effects suppression, approved in
    // specs/render-core/spec.md; don't add others without the maintainer's
    // approval.
#if KS_HAS_NONBLOCKING
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfunction-effects"
#endif
    return std::chrono::steady_clock::now();
#if KS_HAS_NONBLOCKING
#pragma clang diagnostic pop
#endif
}

} // namespace ks::core
