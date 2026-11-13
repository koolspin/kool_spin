// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/miniaudio-devices/spec.md: real-time safety. The
// backend's callback and notification handler are checked at run time by
// RealtimeSanitizer, under the rtsan preset, in the other tests here.

#include <ks/core/realtime.h>

#include <ks/engine/miniaudio_backend.h>

#include <catch2/catch_test_macros.hpp>

namespace {

using ks::engine::DeviceEvent;
using ks::engine::LossDetector;

struct Seen {
    bool fresh = true;
    bool during_stop = true;
    bool after_restart = true;
    bool stopped = false;
    bool rerouted = false;
};

// Every call is checked by function effect analysis at compile time, and by
// RealtimeSanitizer at run time under the rtsan preset.
Seen exercise(LossDetector& stopped, LossDetector& rerouted) noexcept KS_NONBLOCKING {
    Seen seen;
    seen.fresh = stopped.lost();
    stopped.stopping();
    stopped.on_event(DeviceEvent::Stopped);
    seen.during_stop = stopped.lost();
    stopped.starting();
    stopped.on_event(DeviceEvent::Started);
    seen.after_restart = stopped.lost();
    stopped.on_event(DeviceEvent::Stopped);
    seen.stopped = stopped.lost();
    rerouted.on_event(DeviceEvent::Rerouted);
    seen.rerouted = rerouted.lost();
    return seen;
}

} // namespace

TEST_CASE("miniaudio devices are real-time safe", "[realtime]") {
    LossDetector stopped;
    LossDetector rerouted;
    const Seen seen = exercise(stopped, rerouted);
    CHECK_FALSE(seen.fresh);
    CHECK_FALSE(seen.during_stop);
    CHECK_FALSE(seen.after_restart);
    CHECK(seen.stopped);
    CHECK(seen.rerouted);
}
