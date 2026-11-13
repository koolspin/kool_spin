// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/harness/spec.md: playing a script through a
// device, on miniaudio's null backend (requirements 25 to 28). Under the
// rtsan preset, RealtimeSanitizer checks the engine's audio thread here.

#include <ks/engine/miniaudio_backend.h>
#include <ks/harness/run.h>
#include <ks/harness/script.h>
#include <ks/harness/tracks.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <sstream>
#include <string>
#include <thread>

namespace {

using ks::engine::MiniaudioBackends;
using ks::harness::DeviceOptions;
using ks::harness::play_on_device;
using ks::harness::read_script;
using ks::harness::Report;
using ks::harness::Script;
using ks::harness::ScriptError;
using ks::harness::TrackSet;
using ks::harness::test::error_text;

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

/// The null device at 48 kHz, stereo. Blocks of 512 frames last 10.7 ms, so
/// a busy test runner has room before a block counts as late.
const DeviceOptions kNullDevice{.backends = MiniaudioBackends::Null,
                                .sample_rate = 48'000,
                                .channels = 2,
                                .period_frames = 512};

struct Prepared {
    Script script;
    TrackSet tracks;
};

Prepared prepare(const std::string& text) {
    std::expected<Script, ScriptError> script = read_script(text, "/");
    CAPTURE(error_text(script));
    REQUIRE(script.has_value());
    std::expected<TrackSet, std::string> tracks = TrackSet::prepare(*script);
    CAPTURE(error_text(tracks));
    REQUIRE(tracks.has_value());
    return Prepared{.script = std::move(*script), .tracks = std::move(*tracks)};
}

} // namespace

TEST_CASE("Device: playing a script on the null backend", "[harness]") {
    // The last command comes 1 ms before the end: it's still sent, and the
    // harness waits for the engine to read it before stopping.
    const Prepared prepared = prepare("track tone sine 1000 5\n"
                                      "0 load 1 tone\n"
                                      "0 play 1\n"
                                      "0.5 rate 1 1.01\n"
                                      "0.999 fader 1 0.9\n"
                                      "1 end\n");
    const std::atomic<bool> stop{false};
    std::ostringstream out;
    const Clock::time_point started = Clock::now();
    const std::expected<Report, std::string> report =
        play_on_device(prepared.script, prepared.tracks, kNullDevice, stop, out);
    const auto took = std::chrono::duration<double>(Clock::now() - started).count();
    CAPTURE(error_text(report), out.str(), took);
    REQUIRE(report.has_value());

    CHECK(report->played_seconds >= 0.9);
    CHECK(report->played_seconds <= 1.1);
    CHECK(!report->stopped_early);
    CHECK(report->blocks_rendered > 0);
    CHECK(report->late_blocks == 0);
    CHECK(report->non_finite_samples == 0);
    CHECK(report->commands_sent == 3);
    CHECK(report->commands_read == 3);
    CHECK(report->commands_rejected == 0);
    CHECK(report->device_losses == 0);
    CHECK(took < 3.0);

    // It names the device, and shows the output starting to play.
    CHECK(out.str().find("NULL Playback Device") != std::string::npos);
    CHECK(out.str().find("Playing") != std::string::npos);
}

TEST_CASE("Device: stopping early", "[harness]") {
    const Prepared prepared = prepare("track tone sine 1000 20\n"
                                      "0 load 1 tone\n"
                                      "0 play 1\n"
                                      "10 end\n");
    std::atomic<bool> stop{false};
    std::atomic<std::int64_t> stopped_at{0};
    std::thread stopper{[&stop, &stopped_at] {
        std::this_thread::sleep_for(milliseconds{200});
        stopped_at.store(Clock::now().time_since_epoch().count());
        stop.store(true);
    }};
    std::ostringstream out;
    const std::expected<Report, std::string> report =
        play_on_device(prepared.script, prepared.tracks, kNullDevice, stop, out);
    const Clock::time_point returned = Clock::now();
    stopper.join();
    CAPTURE(error_text(report), out.str());
    REQUIRE(report.has_value());

    const Clock::time_point flagged{Clock::duration{stopped_at.load()}};
    const auto after = std::chrono::duration_cast<milliseconds>(returned - flagged);
    CAPTURE(after.count());
    CHECK(after <= milliseconds{500});
    CHECK(report->stopped_early);
    CHECK(report->played_seconds > 0.1);
    CHECK(report->played_seconds < 1.0);
}
