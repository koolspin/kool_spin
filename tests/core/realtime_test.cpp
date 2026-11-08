// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/core/realtime.h>

#include <catch2/catch_test_macros.hpp>

namespace {

int scale(int value, int factor) noexcept KS_NONBLOCKING {
    return value * factor;
}

} // namespace

TEST_CASE("KS_NONBLOCKING functions can be called", "[core][realtime]") {
    REQUIRE(scale(3, 4) == 12);
}

// The real-time guardrails depend on KS_NONBLOCKING expanding to
// [[clang::nonblocking]]. If it silently expanded to nothing on a Clang that
// supports the attribute, function effect analysis and RealtimeSanitizer would
// stop checking without any error.
TEST_CASE("KS_NONBLOCKING is active on Clang 20 and later", "[core][realtime]") {
#if defined(__clang__) && !defined(__apple_build_version__) && __clang_major__ >= 20
    STATIC_REQUIRE(KS_HAS_NONBLOCKING == 1);
#else
    SUCCEED("This compiler has no [[clang::nonblocking]]; Linux CI runs the real-time checks.");
#endif
}
