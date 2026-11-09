// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/tone/options.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <expected>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::expected<ks::tone::Options, std::string> parse(std::initializer_list<std::string_view> args) {
    const std::vector<std::string_view> list{args};
    return ks::tone::parse_options(list);
}

const std::string kFrequencyError = "ks-tone: --frequency must be a number from 20 to 20000";
const std::string kLevelError = "ks-tone: --level must be a number from -60 to -6";
const std::string kDurationError = "ks-tone: --duration must be a number from 0.1 to 60";
const std::string kDeviceError =
    "ks-tone: --device must be a device number shown by --list-devices";

} // namespace

TEST_CASE("parse_options gives the defaults for no arguments", "[tone][options]") {
    const auto options = parse({});
    REQUIRE(options.has_value());
    CHECK(options->backend.empty());
    CHECK(options->settings.frequency_hz == 440.0);
    CHECK(options->settings.level_dbfs == -20.0);
    CHECK(options->settings.fade_seconds == ks::tone::ToneSettings{}.fade_seconds);
    CHECK(options->duration.count() == 2.0);
    CHECK_FALSE(options->device.has_value());
    CHECK_FALSE(options->list_devices);
    CHECK_FALSE(options->help);
}

TEST_CASE("parse_options reads each option", "[tone][options]") {
    const auto options = parse({"--backend", "null", "--frequency", "1000", "--level", "-12",
                                "--duration", "0.5", "--device", "3", "--list-devices"});
    REQUIRE(options.has_value());
    CHECK(options->backend == "null");
    CHECK(options->settings.frequency_hz == 1000.0);
    CHECK(options->settings.level_dbfs == -12.0);
    CHECK(options->duration.count() == 0.5);
    CHECK(options->device == 3U);
    CHECK(options->list_devices);
    CHECK_FALSE(options->help);
}

TEST_CASE("parse_options reads decimals", "[tone][options]") {
    const auto options = parse({"--frequency", "440.5", "--level", "-20.25", "--duration", ".5"});
    REQUIRE(options.has_value());
    CHECK(options->settings.frequency_hz == 440.5);
    CHECK(options->settings.level_dbfs == -20.25);
    CHECK(options->duration.count() == 0.5);
}

TEST_CASE("parse_options stops at --help", "[tone][options]") {
    for (const std::string_view flag : {"-h", "--help"}) {
        const auto options = parse({flag, "--bogus"});
        REQUIRE(options.has_value());
        CHECK(options->help);
    }
}

TEST_CASE("parse_options accepts the range limits", "[tone][options]") {
    CHECK(parse({"--frequency", "20"})->settings.frequency_hz == 20.0);
    CHECK(parse({"--frequency", "20000"})->settings.frequency_hz == 20000.0);
    CHECK(parse({"--level", "-60"})->settings.level_dbfs == -60.0);
    CHECK(parse({"--level", "-6"})->settings.level_dbfs == -6.0);
    CHECK(parse({"--duration", "0.1"})->duration.count() == 0.1);
    CHECK(parse({"--duration", "60"})->duration.count() == 60.0);
}

TEST_CASE("parse_options rejects values out of range", "[tone][options]") {
    CHECK(parse({"--frequency", "19.9"}).error() == kFrequencyError);
    CHECK(parse({"--frequency", "20001"}).error() == kFrequencyError);
    CHECK(parse({"--level", "-5.9"}).error() == kLevelError);
    CHECK(parse({"--level", "-60.1"}).error() == kLevelError);
    CHECK(parse({"--duration", "0.09"}).error() == kDurationError);
    CHECK(parse({"--duration", "60.1"}).error() == kDurationError);
}

TEST_CASE("parse_options rejects anything but a plain decimal", "[tone][options]") {
    const std::string_view value =
        GENERATE(as<std::string_view>{}, "nan", "-nan", "NAN", "inf", "-inf", "infinity", "",
                 "440x", "1e3", "+440", "0x1b8", " 440", "440 ", "-", ".", "-.", "4.4.0", "--440");
    INFO("value: '" << value << "'");
    CHECK(parse({"--frequency", value}).error() == kFrequencyError);
    CHECK(parse({"--level", value}).error() == kLevelError);
    CHECK(parse({"--duration", value}).error() == kDurationError);
}

TEST_CASE("parse_options rejects a missing value", "[tone][options]") {
    for (const std::string_view option :
         {"--backend", "--frequency", "--level", "--duration", "--device"}) {
        CHECK(parse({option}).error() ==
              std::string{"ks-tone: "} + std::string{option} + " needs a value");
    }
    CHECK(parse({"--level", "-12", "--frequency"}).error() == "ks-tone: --frequency needs a value");
}

TEST_CASE("parse_options rejects an invalid device number", "[tone][options]") {
    const std::string_view value =
        GENERATE(as<std::string_view>{}, "-1", "one", "", "+1", "1.0", "3x", " 3", "4294967296");
    INFO("value: '" << value << "'");
    CHECK(parse({"--device", value}).error() == kDeviceError);
}

TEST_CASE("parse_options accepts device numbers from 0", "[tone][options]") {
    CHECK(parse({"--device", "0"})->device == 0U);
    CHECK(parse({"--device", "3"})->device == 3U);
    CHECK(parse({"--device", "4294967295"})->device == 4294967295U);
}

TEST_CASE("parse_options rejects unknown arguments", "[tone][options]") {
    CHECK(parse({"--bogus"}).error() == "ks-tone: unexpected argument '--bogus'");
    CHECK(parse({"--frequency", "440", "extra"}).error() == "ks-tone: unexpected argument 'extra'");
}
