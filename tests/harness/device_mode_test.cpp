// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for play_on_device() on miniaudio's null backend: what it
// prints, and counting freewheel starts as device losses.

#include <ks/engine/miniaudio_backend.h>
#include <ks/harness/run.h>
#include <ks/harness/script.h>
#include <ks/harness/tracks.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <expected>
#include <sstream>
#include <string>

namespace {

using ks::engine::DeviceKey;
using ks::engine::MiniaudioBackends;
using ks::harness::DeviceOptions;
using ks::harness::play_on_device;
using ks::harness::read_script;
using ks::harness::Report;
using ks::harness::Script;
using ks::harness::TrackSet;

const DeviceOptions kNullDevice{.backends = MiniaudioBackends::Null,
                                .sample_rate = 48'000,
                                .channels = 2,
                                .period_frames = 512};

constexpr const char* kShortScript = "track tone sine 440 1\n"
                                     "0 load 1 tone\n"
                                     "0 play 1\n"
                                     "0.1 fader 1 0.5\n"
                                     "0.2 end\n";

struct Played {
    std::expected<Report, std::string> report;
    std::string out;
};

Played play(const DeviceOptions& options) {
    const std::expected<Script, ks::harness::ScriptError> script = read_script(kShortScript, "/");
    REQUIRE(script.has_value());
    const std::expected<TrackSet, std::string> tracks = TrackSet::prepare(*script);
    REQUIRE(tracks.has_value());
    const std::atomic<bool> stop{false};
    std::ostringstream out;
    std::expected<Report, std::string> report =
        play_on_device(*script, *tracks, options, stop, out);
    return Played{.report = std::move(report), .out = out.str()};
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("Device mode prints the device and its state changes", "[harness]") {
    const Played played = play(kNullDevice);
    CAPTURE(played.out);
    REQUIRE(played.report.has_value());

    CHECK(contains(played.out, "Backend: Null\n"));
    CHECK(contains(played.out, "Device: NULL Playback Device\n"));
    CHECK(contains(played.out, "Format: 48000 Hz, 2 channels, period 512 frames"));
    CHECK(contains(played.out, "Output latency: "));
    CHECK(contains(played.out, " s: Playing\n"));
    CHECK(contains(played.out, " s: Stopped\n"));
    // The device is shown before the output starts.
    CHECK(played.out.find("Backend: ") < played.out.find(" s: Playing"));

    CHECK(played.report->commands_sent == 2);
    CHECK(played.report->commands_read == 2);
    CHECK(played.report->device_losses == 0);
    CHECK(!played.report->stopped_early);
    CHECK(played.report->played_seconds > 0.1);
}

TEST_CASE("Device mode counts freewheeling as a device loss", "[harness]") {
    // A key no device has: the source resolves it, but it can't open, so the
    // output freewheels from the start.
    DeviceOptions options = kNullDevice;
    DeviceKey key;
    key.bytes.fill(std::byte{0x5a});
    options.device = key;

    const Played played = play(options);
    CAPTURE(played.out);
    REQUIRE(played.report.has_value());

    CHECK(contains(played.out, "The output device couldn't be opened: "));
    CHECK(contains(played.out, " s: Freewheeling\n"));
    CHECK(played.report->device_losses == 1);
    CHECK(played.report->commands_read == played.report->commands_sent);
}
