// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/core/realtime.h>
#include <ks/engine/audio_backend.h>
#include <ks/engine/block.h>
#include <ks/engine/miniaudio_backend.h>
#include <ks/engine/output_controller.h>
#include <ks/engine/paced_backend.h>
#include <ks/engine/render_core.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <thread>

namespace {

using ks::engine::AudioBackend;
using ks::engine::BackendError;
using ks::engine::Block;
using ks::engine::DeviceSource;
using ks::engine::DriverId;
using ks::engine::Graph;
using ks::engine::Loss;
using ks::engine::MiniaudioBackends;
using ks::engine::MiniaudioConfig;
using ks::engine::open_miniaudio;
using ks::engine::open_paced;
using ks::engine::OutputConfig;
using ks::engine::OutputController;
using ks::engine::OutputState;
using ks::engine::PacedConfig;
using ks::engine::RenderCore;
using ks::engine::SimulatedDevice;
using ks::engine::StreamInfo;

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

class NullGraph final : public Graph {
public:
    void process(const Block& /*block*/) noexcept KS_NONBLOCKING override {}
};

/// Opens, starts, and then never calls back.
class SilentBackend final : public AudioBackend {
public:
    std::expected<void, BackendError> start() override { return {}; }
    void stop() noexcept override {}
    [[nodiscard]] const StreamInfo& info() const noexcept override { return info_; }
    [[nodiscard]] bool lost() const noexcept override { return false; }
    [[nodiscard]] std::uint64_t callbacks() const noexcept override { return 0; }

private:
    StreamInfo info_{.backend = "Silent",
                     .device = "",
                     .sample_rate = 48'000,
                     .channels = 2,
                     .period_frames = 256};
};

/// Opens, but won't start.
class UnstartableBackend final : public AudioBackend {
public:
    std::expected<void, BackendError> start() override {
        return std::unexpected{
            BackendError{.kind = BackendError::Kind::StartFailed, .message = "won't start"}};
    }
    void stop() noexcept override {}
    [[nodiscard]] const StreamInfo& info() const noexcept override { return info_; }
    [[nodiscard]] bool lost() const noexcept override { return false; }
    [[nodiscard]] std::uint64_t callbacks() const noexcept override { return 0; }

private:
    StreamInfo info_{};
};

/// A device source whose next open is chosen by the test.
class ScriptedSource final : public DeviceSource {
public:
    enum class Next : std::uint8_t { Unavailable, Silent, Unstartable, Paced };

    Next next = Next::Unavailable;

    [[nodiscard]] bool available() override { return next != Next::Unavailable; }

    std::expected<std::unique_ptr<AudioBackend>, BackendError> open(RenderCore& core,
                                                                    DriverId driver) override {
        switch (next) {
        case Next::Silent:
            return std::make_unique<SilentBackend>();
        case Next::Unstartable:
            return std::make_unique<UnstartableBackend>();
        case Next::Paced: {
            auto opened = open_paced(core, PacedConfig{.sample_rate = 48'000,
                                                       .channels = 2,
                                                       .period_frames = 256,
                                                       .driver = driver});
            if (!opened) {
                return std::unexpected{opened.error()};
            }
            return std::unique_ptr<AudioBackend>{std::move(*opened)};
        }
        case Next::Unavailable:
            break;
        }
        return std::unexpected{
            BackendError{.kind = BackendError::Kind::OpenFailed, .message = "unavailable"}};
    }
};

/// Polls every 5 ms until the controller is in `state`, or `timeout` passes.
bool poll_until(OutputController& controller, OutputState state, milliseconds timeout) {
    const Clock::time_point deadline = Clock::now() + timeout;
    while (controller.state() != state) {
        if (Clock::now() >= deadline) {
            return false;
        }
        controller.poll();
        std::this_thread::sleep_for(milliseconds{5});
    }
    return true;
}

const OutputConfig kQuickRetries{.stall_timeout = milliseconds{100},
                                 .retry_interval = milliseconds{50}};

} // namespace

TEST_CASE("A returning device that never calls back is dropped", "[engine][output]") {
    NullGraph graph;
    RenderCore core{graph};
    ScriptedSource source;
    OutputController controller{core, source, kQuickRetries};

    controller.start();
    REQUIRE(controller.state() == OutputState::Freewheeling);

    // It opens and starts, but never calls back: dropped after the stall
    // timeout, while the freewheel driver keeps the clock going.
    source.next = ScriptedSource::Next::Silent;
    REQUIRE(poll_until(controller, OutputState::Attaching, milliseconds{1000}));
    source.next = ScriptedSource::Next::Unavailable;
    REQUIRE(poll_until(controller, OutputState::Freewheeling, milliseconds{1000}));
    const std::uint64_t frames = core.frames_rendered();
    std::this_thread::sleep_for(milliseconds{50});
    REQUIRE(core.frames_rendered() > frames);

    // A device that works takes over.
    source.next = ScriptedSource::Next::Paced;
    REQUIRE(poll_until(controller, OutputState::Playing, milliseconds{1000}));
    controller.stop();
}

TEST_CASE("A device that won't start leaves the controller freewheeling", "[engine][output]") {
    NullGraph graph;
    RenderCore core{graph};
    ScriptedSource source;
    source.next = ScriptedSource::Next::Unstartable;
    OutputController controller{core, source, kQuickRetries};

    controller.start();
    REQUIRE(controller.state() == OutputState::Freewheeling);
    for (int i = 0; i < 20; ++i) {
        controller.poll();
        std::this_thread::sleep_for(milliseconds{5});
        REQUIRE(controller.state() == OutputState::Freewheeling);
    }
    REQUIRE(core.frames_rendered() > 0);
}

TEST_CASE("Stopping while freewheeling fades out first", "[engine][output]") {
    NullGraph graph;
    RenderCore core{graph};
    ScriptedSource source;
    OutputController controller{core, source, kQuickRetries};

    controller.start();
    REQUIRE(controller.state() == OutputState::Freewheeling);
    std::this_thread::sleep_for(milliseconds{20});
    const std::uint64_t before = core.frames_rendered();
    controller.stop();
    REQUIRE(core.silenced());
    // The default 10 ms fade at the freewheel driver's 48 kHz.
    REQUIRE(core.frames_rendered() - before >= 480);
}

TEST_CASE("Stopping gives up on the fade out if nothing renders it", "[engine][output]") {
    NullGraph graph;
    RenderCore core{graph};
    ScriptedSource source;
    source.next = ScriptedSource::Next::Silent;
    OutputController controller{core, source, kQuickRetries};

    controller.start();
    REQUIRE(controller.state() == OutputState::Playing);
    const Clock::time_point before = Clock::now();
    controller.stop();
    const Clock::duration took = Clock::now() - before;
    REQUIRE(controller.state() == OutputState::Stopped);
    REQUIRE_FALSE(core.silenced());
    // The fade time, 10 ms, plus 100 ms, and not much more.
    REQUIRE(took >= milliseconds{110});
    REQUIRE(took < milliseconds{1000});
}

TEST_CASE("A paced backend opened on an unplugged device never renders", "[engine][paced]") {
    NullGraph graph;
    RenderCore core{graph};
    SimulatedDevice device;
    device.unplug(Loss::Reported);
    auto opened = open_paced(
        core,
        PacedConfig{.sample_rate = 48'000, .channels = 2, .period_frames = 256, .device = &device});
    REQUIRE(opened.has_value());
    REQUIRE((*opened)->start().has_value());
    REQUIRE((*opened)->start().has_value()); // starting twice is harmless
    std::this_thread::sleep_for(milliseconds{50});
    REQUIRE((*opened)->callbacks() == 0);
    REQUIRE(core.frames_rendered() == 0);
    // It never saw the device go, so it has nothing to report.
    REQUIRE_FALSE((*opened)->lost());
    (*opened)->stop();
}

TEST_CASE("The miniaudio backend counts callbacks and renders as its driver",
          "[engine][miniaudio]") {
    constexpr DriverId kDriver{5};
    NullGraph graph;
    RenderCore core{graph};
    auto opened = open_miniaudio(core, MiniaudioConfig{.backends = MiniaudioBackends::Null,
                                                       .channels = 2,
                                                       .sample_rate = 48'000,
                                                       .period_frames = 0,
                                                       .driver = kDriver});
    REQUIRE(opened.has_value());
    AudioBackend& backend = **opened;
    REQUIRE_FALSE(backend.lost());

    // Not active: it calls back, but nothing renders.
    REQUIRE(backend.start().has_value());
    const Clock::time_point deadline = Clock::now() + milliseconds{2000};
    while (backend.callbacks() == 0 && Clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds{1});
    }
    REQUIRE(backend.callbacks() > 0);
    REQUIRE(core.frames_rendered() == 0);

    // Active: it renders.
    core.activate(kDriver);
    while (core.frames_rendered() == 0 && Clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds{1});
    }
    backend.stop();
    REQUIRE(core.frames_rendered() > 0);
}
