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
        ks::tone::play_tone("null", ks::tone::ToneSettings{}, std::chrono::milliseconds{200},
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
}

TEST_CASE("play_tone rejects an unknown backend", "[tone][audio]") {
    const auto report =
        ks::tone::play_tone("nonexistent", ks::tone::ToneSettings{}, std::chrono::milliseconds{0});
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error() == "unknown backend 'nonexistent'");
}
