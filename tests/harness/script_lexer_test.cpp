// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the script reader's lexer: tokens, lines, and the number
// syntax (specs/harness/spec.md requirements 4 and 5).

#include "script_lexer.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace {

using ks::harness::lexer::is_track_name;
using ks::harness::lexer::parse_amount;
using ks::harness::lexer::parse_db;
using ks::harness::lexer::parse_value;
using ks::harness::lexer::parse_whole;
using ks::harness::lexer::split_lines;
using ks::harness::lexer::Token;
using ks::harness::lexer::tokenize;

TEST_CASE("lexer: whole numbers") {
    CHECK(parse_whole("0", 10) == 0U);
    CHECK(parse_whole("007", 10) == 7U);
    CHECK(parse_whole("10", 10) == 10U);
    CHECK(parse_whole("11", 10) == std::nullopt);
    CHECK(parse_whole("99999999999999999999999", UINT64_MAX) == std::nullopt);
    for (const std::string_view bad : {"", "-1", "+1", "1.0", "1e3", " 1", "x"}) {
        CAPTURE(bad);
        CHECK(parse_whole(bad, 100) == std::nullopt);
    }
}

TEST_CASE("lexer: amounts, values, and dB") {
    CHECK(parse_amount("12") == 12.0);
    CHECK(parse_amount("0.25") == 0.25);
    CHECK(parse_value("-0.5") == -0.5);
    CHECK(parse_value("1.02") == 1.02);
    CHECK(parse_db("-6") == -6.0);
    const double minus_inf = parse_db("-inf").value_or(0.0);
    CHECK(std::isinf(minus_inf));
    CHECK(minus_inf < 0.0);

    for (const std::string_view bad : {"", ".5", "5.", ".", "1..2", "1.2.3", "+1", "1e3", "1E3",
                                       "nan", "inf", "0x10", "-", "--1", "1-", " 1", "1 "}) {
        CAPTURE(bad);
        CHECK(parse_amount(bad) == std::nullopt);
        CHECK(parse_value(bad) == std::nullopt);
        CHECK(parse_db(bad) == std::nullopt);
    }
    CHECK(parse_amount("-1") == std::nullopt);
    CHECK(parse_value("-inf") == std::nullopt);
    CHECK(parse_db("inf") == std::nullopt);
    CHECK(parse_db("+inf") == std::nullopt);
    // Too big for a double.
    CHECK(parse_amount(std::string(400, '9')) == std::nullopt);
}

TEST_CASE("lexer: tokens") {
    SECTION("spaces, tabs, and comments") {
        const auto tokens = tokenize(" 1\tplay  1 # a comment");
        REQUIRE(tokens.has_value());
        CHECK(*tokens == std::vector<Token>{{.text = "1"}, {.text = "play"}, {.text = "1"}});
        CHECK(tokenize("1 play 1#x")->size() == 3);
        CHECK(tokenize("# only")->empty());
        CHECK(tokenize("")->empty());
    }
    SECTION("a quoted path keeps spaces and #") {
        const auto tokens = tokenize(R"(track a file "a # b.wav"# c)");
        REQUIRE(tokens.has_value());
        REQUIRE(tokens->size() == 4);
        CHECK(tokens->back() == Token{.text = "a # b.wav", .quoted = true});
    }
    SECTION("bad quotes") {
        CHECK(!tokenize(R"(file "x.wav)").has_value());
        CHECK(!tokenize(R"(file "")").has_value());
        CHECK(!tokenize(R"(file "x"y)").has_value());
        CHECK(!tokenize(R"(file x"y")").has_value());
    }
}

TEST_CASE("lexer: lines") {
    CHECK(split_lines("").empty());
    CHECK(split_lines("a\r\nb\n\nc") == std::vector<std::string_view>{"a", "b", "", "c"});
    CHECK(split_lines("a\n") == std::vector<std::string_view>{"a"});
    CHECK(split_lines("a\r") == std::vector<std::string_view>{"a"});
}

TEST_CASE("lexer: track names") {
    for (const std::string_view good : {"a", "_", "Kick-2", "a_b-c9"}) {
        CAPTURE(good);
        CHECK(is_track_name(good));
    }
    for (const std::string_view bad : {"", "9lives", "-a", "a.b", "a b", "é"}) {
        CAPTURE(bad);
        CHECK(!is_track_name(bad));
    }
}

} // namespace
