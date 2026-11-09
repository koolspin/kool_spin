// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/core/realtime.h>

#include <catch2/catch_test_macros.hpp>

namespace {

int scale(int value, int factor) noexcept KS_NONBLOCKING {
    return value * factor;
}

// True when the compiler implements function effect analysis, which comes
// with [[clang::nonblocking]]: Clang 20+ and AppleClang 17+.
constexpr bool kCompilerChecksFunctionEffects =
#ifdef __has_warning
#if __has_warning("-Wfunction-effects")
    true;
#else
    false;
#endif
#else
    false;
#endif

} // namespace

TEST_CASE("KS_NONBLOCKING functions can be called", "[core][realtime]") {
    REQUIRE(scale(3, 4) == 12);
}

// The real-time guardrails depend on KS_NONBLOCKING expanding to
// [[clang::nonblocking]]. If it silently expanded to nothing on a compiler
// that supports the attribute, function effect analysis and RealtimeSanitizer
// would stop checking without any error.
TEST_CASE("KS_NONBLOCKING is active wherever the compiler supports it", "[core][realtime]") {
    STATIC_REQUIRE((KS_HAS_NONBLOCKING == 1) == kCompilerChecksFunctionEffects);
}
