// SPDX-License-Identifier: GPL-3.0-or-later

#include "numbers.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

using ks::play_app::is_decimal;
using ks::play_app::NegativeInfinity;
using ks::play_app::parse_index;
using ks::play_app::parse_real;
using ks::play_app::parse_whole;

// In loops, these tests write CHECK(!x) rather than CHECK_FALSE(x): there,
// clang-analyzer takes the flags CHECK_FALSE combines for an enum value out
// of range (optin.core.EnumCastOutOfRange).

TEST_CASE("Whole numbers are digits only", "[play_app][numbers]") {
    CHECK(parse_whole("0") == std::optional<std::uint64_t>{0});
    CHECK(parse_whole("007") == std::optional<std::uint64_t>{7});
    CHECK(parse_whole("60000") == std::optional<std::uint64_t>{60'000});
    CHECK(parse_whole("18446744073709551615") ==
          std::optional<std::uint64_t>{std::numeric_limits<std::uint64_t>::max()});

    for (const std::string_view bad :
         {"", "-1", "+1", " 1", "1 ", "1.5", "1e3", "0x10", "x", "18446744073709551616"}) {
        CAPTURE(bad);
        CHECK(!parse_whole(bad).has_value());
    }
}

TEST_CASE("Decks and channels are 0 to 3", "[play_app][numbers]") {
    CHECK(parse_index("0") == std::optional<std::uint32_t>{0});
    CHECK(parse_index("3") == std::optional<std::uint32_t>{3});
    for (const std::string_view bad : {"4", "-1", "1.5", " 1", "", "4294967296"}) {
        CAPTURE(bad);
        CHECK(!parse_index(bad).has_value());
    }
}

TEST_CASE("The decimal syntax", "[play_app][numbers]") {
    for (const std::string_view good :
         {"0", "-1", "+1", "1.5", "-0.25", ".5", "5.", "1e3", "1E-3", "-2.5e+10", "007"}) {
        CAPTURE(good);
        CHECK(is_decimal(good));
    }
    for (const std::string_view bad :
         {"", "+", "-", ".", "e3", "1e", "1e+", "1x", "0x10", "inf", "nan", "-inf", " 1", "1 ",
          "1..2", "1.2.3", "--1", "1e3.5", "infinity"}) {
        CAPTURE(bad);
        CHECK(!is_decimal(bad));
    }
}

TEST_CASE("Reals", "[play_app][numbers]") {
    CHECK(parse_real("1.02", NegativeInfinity::Refused) == std::optional<double>{1.02});
    CHECK(parse_real("-1", NegativeInfinity::Refused) == std::optional<double>{-1.0});
    CHECK(parse_real("+0.5", NegativeInfinity::Refused) == std::optional<double>{0.5});
    CHECK(parse_real("2.5e3", NegativeInfinity::Refused) == std::optional<double>{2500.0});

    SECTION("-inf only where allowed") {
        CHECK(parse_real("-inf", NegativeInfinity::Allowed) ==
              std::optional<double>{-std::numeric_limits<double>::infinity()});
        CHECK_FALSE(parse_real("-inf", NegativeInfinity::Refused).has_value());
        for (const std::string_view bad : {"inf", "+inf", "nan", "-nan", "-INF", "-infinity"}) {
            CAPTURE(bad);
            CHECK(!parse_real(bad, NegativeInfinity::Allowed).has_value());
        }
    }

    SECTION("an overflow is not an infinity") {
        CHECK_FALSE(parse_real("1e999", NegativeInfinity::Allowed).has_value());
        CHECK_FALSE(parse_real("-1e999", NegativeInfinity::Allowed).has_value());
    }

    SECTION("anything else") {
        for (const std::string_view bad : {"", "1x", "0x10", " 1", "1 ", "abc"}) {
            CAPTURE(bad);
            CHECK(!parse_real(bad, NegativeInfinity::Allowed).has_value());
        }
    }
}
