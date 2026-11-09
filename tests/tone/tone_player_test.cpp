// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/tone/tone_player.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <optional>

// miniaudio's null backend runs a real audio thread that calls the render
// callback on schedule, without any audio hardware. Under the rtsan preset,
// any real-time violation in the callback path fails this test.
TEST_CASE("play_tone plays through the null backend", "[tone][audio]") {
    std::optional<ks::tone::DeviceInfo> started;
    const auto report =
        ks::tone::play_tone("null", ks::tone::ToneSettings{}, std::chrono::milliseconds{200}, {},
                            [&started](const ks::tone::DeviceInfo& info) { started = info; });

    REQUIRE(report.has_value());
    REQUIRE(started.has_value());
    CHECK(report->device.backend == "Null");
    CHECK(report->device.sample_rate > 0);

    // Roughly 200 ms of audio, allowing for scheduling slack.
    const auto expected_frames = report->device.sample_rate / 5;
    CHECK(report->frames_rendered >= expected_frames / 2);

    const auto amplitude = static_cast<float>(std::pow(10.0, -20.0 / 20.0));
    CHECK(report->peak == Catch::Approx(amplitude).epsilon(1e-3));
    CHECK(report->measured_hz == Catch::Approx(440.0).epsilon(1e-3));
}

TEST_CASE("play_tone plays the given settings for a fractional duration", "[tone][audio]") {
    const ks::tone::ToneSettings settings{.frequency_hz = 1000.0, .level_dbfs = -12.0};
    const auto report = ks::tone::play_tone("null", settings, std::chrono::duration<double>{0.1});

    REQUIRE(report.has_value());
    // Roughly 100 ms of audio plus the fade-out, allowing for scheduling slack.
    const auto expected_frames = report->device.sample_rate / 10;
    CHECK(report->frames_rendered >= expected_frames / 2);
    CHECK(report->frames_rendered < report->device.sample_rate / 2); // not rounded up to 1 s

    const auto amplitude = static_cast<float>(std::pow(10.0, -12.0 / 20.0));
    CHECK(report->peak == Catch::Approx(amplitude).epsilon(1e-3));
    CHECK(report->measured_hz == Catch::Approx(1000.0).epsilon(1e-3));
}

TEST_CASE("play_tone rejects an unknown backend", "[tone][audio]") {
    const auto report =
        ks::tone::play_tone("nonexistent", ks::tone::ToneSettings{}, std::chrono::milliseconds{0});
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().kind == ks::tone::PlayError::Kind::Runtime);
    CHECK(report.error().message == "unknown backend 'nonexistent'");
}

TEST_CASE("play_tone plays on a chosen device", "[tone][audio]") {
    const auto report = ks::tone::play_tone("null", ks::tone::ToneSettings{},
                                            std::chrono::duration<double>{0.1}, 0U);

    REQUIRE(report.has_value());
    CHECK(report->device.backend == "Null");
    CHECK(report->device.device == "NULL Playback Device");
    CHECK(report->frames_rendered > 0);
}

TEST_CASE("play_tone rejects a device number past the end", "[tone][audio]") {
    bool started = false;
    const auto report = ks::tone::play_tone(
        "null", ks::tone::ToneSettings{}, std::chrono::duration<double>{0.1}, 1U,
        [&started](const ks::tone::DeviceInfo& /*info*/) { started = true; });

    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().kind == ks::tone::PlayError::Kind::Usage);
    CHECK(report.error().message == "no output device 1 on Null");
    CHECK_FALSE(started);
}

TEST_CASE("list_devices lists the null backend's one device", "[tone][audio]") {
    const auto list = ks::tone::list_devices("null");

    REQUIRE(list.has_value());
    CHECK(list->backend == "Null");
    REQUIRE(list->devices.size() == 1);
    CHECK(list->devices[0].name == "NULL Playback Device");
    CHECK(list->devices[0].is_default);
}

TEST_CASE("list_devices rejects an unknown backend", "[tone][audio]") {
    const auto list = ks::tone::list_devices("nonexistent");
    REQUIRE_FALSE(list.has_value());
    CHECK(list.error() == "unknown backend 'nonexistent'");
}
