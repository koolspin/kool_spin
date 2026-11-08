// SPDX-License-Identifier: GPL-3.0-or-later
//
// Canary for the rtsan preset. Each mode breaks a real-time rule inside a
// KS_NONBLOCKING function, and RealtimeSanitizer must report it. CTest passes
// a mode only when the report appears in the output (see tests/CMakeLists.txt),
// so if RealtimeSanitizer were missing or misconfigured, these tests would fail.

#include <ks/core/realtime.h>

#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>

// Function effect analysis rejects these violations at compile time. Silence
// it here, and only here, so the violations reach the run-time check.
#if KS_HAS_NONBLOCKING
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfunction-effects"
#endif

namespace {

[[gnu::noinline]] std::unique_ptr<int> allocate(int value) noexcept KS_NONBLOCKING {
    return std::make_unique<int>(value);
}

[[gnu::noinline]] void lock(std::mutex& mutex) noexcept KS_NONBLOCKING {
    const std::scoped_lock guard{mutex};
}

} // namespace

#if KS_HAS_NONBLOCKING
#pragma clang diagnostic pop
#endif

int main(int argc, char* argv[]) {
    const std::span args{argv, static_cast<std::size_t>(argc)};
    const std::string_view mode = args.size() > 1 ? args[1] : "";

    if (mode == "allocate") {
        const auto value = allocate(42);
        std::puts("allocated memory without a RealtimeSanitizer report");
    } else if (mode == "lock") {
        std::mutex mutex;
        lock(mutex);
        std::puts("locked a mutex without a RealtimeSanitizer report");
    } else {
        std::fputs("usage: ks_rtsan_canary allocate|lock\n", stderr);
        return 2;
    }
    return 0;
}
