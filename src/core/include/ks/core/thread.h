// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Naming the calling thread from the audio thread
/// (specs/health-checks/spec.md requirement 4).

#include <ks/core/realtime.h>

#include <pthread.h>

namespace ks::core {

/// `pthread_self()`, callable from KS_NONBLOCKING code.
[[nodiscard]] inline pthread_t current_thread() noexcept KS_NONBLOCKING {
    // pthread_self() isn't annotated, so function effect analysis rejects the
    // call. It doesn't block: it reads the thread pointer, with no system
    // call, on Linux and macOS. RealtimeSanitizer still checks it at run
    // time. This suppression, like monotonic_now()'s, is approved in
    // specs/health-checks/spec.md; don't add others without the
    // maintainer's approval.
#if KS_HAS_NONBLOCKING
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfunction-effects"
#endif
    return ::pthread_self();
#if KS_HAS_NONBLOCKING
#pragma clang diagnostic pop
#endif
}

} // namespace ks::core
