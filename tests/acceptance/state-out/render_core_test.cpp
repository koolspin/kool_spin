// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/state-out/spec.md: track IDs, output latency,
// and the render core's timing, output state, and rendered hook
// (requirements 9 to 15).

#include <ks/engine/render_core.h>

#include <ks/core/clock.h>
#include <ks/core/realtime.h>
#include <ks/engine/block.h>
#include <ks/engine/interleave.h>
#include <ks/engine/output_controller.h>
#include <ks/engine/paced_backend.h>
#include <ks/engine/track.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <thread>
#include <vector>

namespace {

using ks::engine::Block;
using ks::engine::DriverId;
using ks::engine::frames_to_duration;
using ks::engine::Graph;
using ks::engine::kDefaultDriver;
using ks::engine::OutputState;
using ks::engine::RenderCore;
using ks::engine::RenderResult;
using ks::engine::RenderStats;
using ks::engine::state_test::Channels;
using ks::engine::state_test::kBlock;
using ks::engine::state_test::kRate;
using ks::engine::state_test::kStart;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;

/// What a graph was asked to render.
struct Seen {
    std::size_t frames = 0;
    std::uint32_t sample_rate = 0;
    ks::engine::state_test::Clock::time_point host_time;
    nanoseconds output_latency{};
    bool resumed = false;
};

/// Records the blocks it renders, up to 16.
class RecordingGraph final : public Graph {
public:
    void process(const Block& block) noexcept KS_NONBLOCKING override {
        if (count_ < records_.size()) {
            std::span{records_}[count_] = Seen{.frames = block.frames,
                                               .sample_rate = block.sample_rate,
                                               .host_time = block.host_time,
                                               .output_latency = block.output_latency,
                                               .resumed = block.resumed};
        }
        ++count_;
    }

    [[nodiscard]] std::span<const Seen> records() const {
        return std::span{records_}.first(std::min(count_, records_.size()));
    }

private:
    std::array<Seen, 16> records_{};
    std::size_t count_ = 0;
};

/// What to write into a block.
enum class Content : std::uint8_t { Quiet, NaN, Loud };

/// Writes what it's told to, and records each call to rendered(). From
/// inside rendered(), it tries to render another block through the same
/// core, which must be skipped: the token is still held.
class HookGraph final : public Graph {
public:
    void process(const Block& block) noexcept KS_NONBLOCKING override {
        for (const std::span<float> channel : block.channels) {
            for (std::size_t f = 0; f < block.frames; ++f) {
                float& sample = channel[f];
                switch (content_) {
                case Content::Quiet:
                    sample = 0.1F;
                    break;
                case Content::NaN:
                    sample = f == 0 ? std::numeric_limits<float>::quiet_NaN() : 0.1F;
                    break;
                case Content::Loud:
                    sample = 2.0F;
                    break;
                }
            }
        }
        processed_seen_ = Seen{.frames = block.frames,
                               .sample_rate = block.sample_rate,
                               .host_time = block.host_time,
                               .output_latency = block.output_latency,
                               .resumed = block.resumed};
    }

    void rendered(const Block& block, const RenderStats& stats) noexcept KS_NONBLOCKING override {
        ++calls_;
        hook_seen_ = Seen{.frames = block.frames,
                          .sample_rate = block.sample_rate,
                          .host_time = block.host_time,
                          .output_latency = block.output_latency,
                          .resumed = block.resumed};
        stats_ = stats;
        if (core_ != nullptr) {
            const Block nested{.channels = nested_channels_.views(),
                               .frames = kBlock,
                               .sample_rate = kRate,
                               .host_time = {},
                               .output_latency = {},
                               .resumed = false};
            nested_ = core_->try_render(nested, kDefaultDriver);
        }
    }

    void set_content(Content content) { content_ = content; }
    void set_core(RenderCore& core) { core_ = &core; }

    [[nodiscard]] std::size_t calls() const { return calls_; }
    [[nodiscard]] const Seen& processed() const { return processed_seen_; }
    [[nodiscard]] const Seen& hooked() const { return hook_seen_; }
    [[nodiscard]] const RenderStats& stats() const { return stats_; }
    [[nodiscard]] RenderResult nested() const { return nested_; }

private:
    Content content_ = Content::Quiet;
    RenderCore* core_ = nullptr;
    Channels nested_channels_;
    std::size_t calls_ = 0;
    Seen processed_seen_{};
    Seen hook_seen_{};
    RenderStats stats_{};
    RenderResult nested_ = RenderResult::Rendered;
};

/// The render core's values, read through its accessors.
RenderStats accessors(const RenderCore& core) {
    return RenderStats{.blocks_rendered = core.blocks_rendered(),
                       .frames_rendered = core.frames_rendered(),
                       .non_finite_samples = core.non_finite_samples(),
                       .limited_frames = core.limited_frames(),
                       .late_blocks = core.late_blocks(),
                       .render_time = core.render_time(),
                       .render_load = core.render_load(),
                       .peak_render_load = core.peak_render_load(),
                       .output_state = core.output_state(),
                       .muted = core.muted(),
                       .silenced = core.silenced()};
}

bool same(const Seen& a, const Seen& b) {
    return a.frames == b.frames && a.sample_rate == b.sample_rate && a.host_time == b.host_time &&
           a.output_latency == b.output_latency && a.resumed == b.resumed;
}

/// Spins in process() for `factor` times the block's duration.
class SpinGraph final : public Graph {
public:
    void process(const Block& block) noexcept KS_NONBLOCKING override {
        const auto start = ks::core::monotonic_now();
        const auto spin = std::chrono::duration_cast<nanoseconds>(
            frames_to_duration(block.frames, block.sample_rate) * factor_);
        while (ks::core::monotonic_now() - start < spin) {
        }
    }

    void set_factor(std::int64_t factor) { factor_ = factor; }

private:
    std::int64_t factor_ = 0;
};

/// Renders one block of `frames` frames through `core`.
RenderResult render(RenderCore& core, const Channels& channels, std::size_t frames,
                    ks::engine::state_test::Clock::time_point host_time = kStart,
                    nanoseconds latency = {}, DriverId driver = kDefaultDriver) {
    return core.try_render(Block{.channels = channels.views(),
                                 .frames = frames,
                                 .sample_rate = kRate,
                                 .host_time = host_time,
                                 .output_latency = latency,
                                 .resumed = false},
                           driver);
}

} // namespace

TEST_CASE("Track: an ID", "[state][track]") {
    ks::engine::state_test::IdTrack with_id{100, 42};
    REQUIRE(with_id.track().id() == 42);

    const std::vector<float> samples(200, 0.0F);
    const ks::engine::Track without_id{samples, kRate};
    REQUIRE(without_id.id() == 0);
}

TEST_CASE("Render interleaved: the output latency reaches every block", "[state][interleave]") {
    RecordingGraph graph;
    RenderCore core{graph};
    std::array<std::vector<float>, 2> storage{std::vector<float>(ks::engine::kMaxBlockFrames),
                                              std::vector<float>(ks::engine::kMaxBlockFrames)};
    const std::array<std::span<float>, 2> scratch{std::span{storage[0]}, std::span{storage[1]}};
    std::vector<float> interleaved(std::size_t{1'000} * 2);

    REQUIRE(ks::engine::render_interleaved(core, scratch, interleaved, 2, kRate, kStart,
                                           kDefaultDriver, milliseconds{3}) == 1'000);
    REQUIRE(ks::engine::render_interleaved(core, scratch, interleaved, 2, kRate, kStart) == 1'000);

    const std::span<const Seen> records = graph.records();
    REQUIRE(records.size() == 4);
    REQUIRE(records[0].frames == ks::engine::kMaxBlockFrames);
    REQUIRE(records[1].frames == 1'000 - ks::engine::kMaxBlockFrames);
    REQUIRE(records[0].output_latency == milliseconds{3});
    REQUIRE(records[1].output_latency == milliseconds{3});
    REQUIRE(records[2].output_latency == nanoseconds{0});
    REQUIRE(records[3].output_latency == nanoseconds{0});
}

TEST_CASE("Render core: the graph hears about each rendered block", "[state][render-core]") {
    HookGraph graph;
    RenderCore core{graph};
    graph.set_core(core);
    const Channels channels;
    std::size_t rendered = 0;
    auto time = kStart;

    // Renders a block and checks what the hook was told.
    const auto check_block = [&](std::size_t frames, nanoseconds latency) {
        CAPTURE(rendered, frames);
        REQUIRE(render(core, channels, frames, time, latency) == RenderResult::Rendered);
        ++rendered;
        time += frames_to_duration(frames, kRate);
        REQUIRE(graph.calls() == rendered);
        // The hook gets the block the graph rendered, and the core's values.
        REQUIRE(same(graph.hooked(), graph.processed()));
        REQUIRE(graph.hooked().output_latency == latency);
        REQUIRE(graph.stats() == accessors(core));
        // The token was still held.
        REQUIRE(graph.nested() == RenderResult::Skipped);
    };

    check_block(kBlock, milliseconds{2});
    REQUIRE(graph.hooked().resumed);
    REQUIRE(graph.stats().blocks_rendered == 1);
    REQUIRE(graph.stats().frames_rendered == kBlock);
    check_block(100, nanoseconds{0});
    REQUIRE_FALSE(graph.hooked().resumed);
    REQUIRE(graph.stats().frames_rendered == kBlock + 100);

    graph.set_content(Content::NaN);
    check_block(kBlock, milliseconds{1});
    REQUIRE(graph.stats().non_finite_samples > 0);

    // The NaN restarted the safety stage's fade in: the limiter acts once the
    // fade has come up far enough.
    graph.set_content(Content::Loud);
    for (std::size_t i = 0; i < 20 && graph.stats().limited_frames == 0; ++i) {
        check_block(kBlock, milliseconds{1});
    }
    REQUIRE(graph.stats().limited_frames > 0);

    graph.set_content(Content::Quiet);
    core.set_muted(true);
    core.set_output_state(OutputState::Freewheeling);
    check_block(kBlock, milliseconds{1});
    REQUIRE(graph.stats().muted);
    REQUIRE(graph.stats().output_state == OutputState::Freewheeling);
    for (std::size_t i = 0; i < 20 && !graph.stats().silenced; ++i) {
        check_block(kBlock, milliseconds{1});
    }
    REQUIRE(graph.stats().silenced);

    // Skipped and rejected blocks don't call the hook.
    core.activate(DriverId{3});
    REQUIRE(render(core, channels, kBlock) == RenderResult::Skipped);
    core.activate(kDefaultDriver);
    REQUIRE(render(core, channels, 0) == RenderResult::Rejected);
    REQUIRE(graph.calls() == rendered);
    REQUIRE(core.blocks_rendered() == rendered);
}

TEST_CASE("Render core: load and late blocks", "[state][render-core]") {
    SpinGraph graph;
    RenderCore core{graph};
    const Channels channels;
    REQUIRE(core.blocks_rendered() == 0);
    REQUIRE(core.render_load() == 0.0);
    REQUIRE(core.peak_render_load() == 0.0);
    REQUIRE(core.render_time() == nanoseconds{0});

    double highest = 0.0;
    const auto block = [&](std::size_t frames) {
        REQUIRE(render(core, channels, frames) == RenderResult::Rendered);
        highest = std::max(highest, core.render_load());
        REQUIRE(core.peak_render_load() >= highest);
    };

    // A graph that does nothing is never late.
    const auto big = frames_to_duration(ks::engine::kMaxBlockFrames, kRate);
    for (std::size_t i = 0; i < 20; ++i) {
        block(ks::engine::kMaxBlockFrames);
        REQUIRE(core.render_time() > nanoseconds{0});
        REQUIRE(core.render_time() < big);
        REQUIRE(core.render_load() < 1.0);
    }
    REQUIRE(core.late_blocks() == 0);
    REQUIRE(core.peak_render_load() < 1.0);

    // One that spins for twice its block's duration always is.
    graph.set_factor(2);
    const auto small = frames_to_duration(kBlock, kRate);
    for (std::size_t i = 0; i < 5; ++i) {
        block(kBlock);
        REQUIRE(core.late_blocks() == i + 1);
        REQUIRE(core.render_time() >= 2 * small);
        REQUIRE(core.render_load() >= 2.0);
    }
    REQUIRE(core.peak_render_load() >= 2.0);

    // Fast again: no more late blocks, and the peak stays.
    graph.set_factor(0);
    for (std::size_t i = 0; i < 5; ++i) {
        block(ks::engine::kMaxBlockFrames);
        REQUIRE(core.render_load() < 1.0);
    }
    REQUIRE(core.late_blocks() == 5);
    REQUIRE(core.peak_render_load() >= 2.0);
    REQUIRE(core.blocks_rendered() == 30);
}

TEST_CASE("Render core: the output state follows the controller", "[state][render-core]") {
    using ks::engine::Loss;
    using ks::engine::OutputConfig;
    using ks::engine::OutputController;
    using ks::engine::PacedConfig;
    using ks::engine::PacedDeviceSource;
    using ks::engine::SimulatedDevice;

    SpinGraph graph;
    RenderCore core{graph};
    REQUIRE(core.output_state() == OutputState::Stopped);
    core.set_output_state(OutputState::Attaching);
    REQUIRE(core.output_state() == OutputState::Attaching);
    core.set_output_state(OutputState::Stopped);
    REQUIRE(core.output_state() == OutputState::Stopped);

    SimulatedDevice device;
    PacedDeviceSource source{
        device, PacedConfig{.sample_rate = kRate, .channels = 2, .period_frames = 256}};
    OutputController controller{
        core, source,
        OutputConfig{.stall_timeout = milliseconds{100}, .retry_interval = milliseconds{200}}};

    // After every controller call, the core's state is the controller's.
    bool always_matched = true;
    const auto poll_until = [&](OutputState target, milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            controller.poll();
            always_matched = always_matched && core.output_state() == controller.state();
            if (controller.state() == target) {
                return true;
            }
            std::this_thread::sleep_for(milliseconds{5});
        }
        return false;
    };

    controller.start();
    REQUIRE(controller.state() == OutputState::Playing);
    REQUIRE(core.output_state() == OutputState::Playing);

    device.unplug(Loss::Silent);
    REQUIRE(poll_until(OutputState::Freewheeling, milliseconds{1'000}));
    REQUIRE(core.output_state() == OutputState::Freewheeling);

    device.plug_in();
    REQUIRE(poll_until(OutputState::Playing, milliseconds{2'000}));
    REQUIRE(core.output_state() == OutputState::Playing);

    controller.stop();
    REQUIRE(controller.state() == OutputState::Stopped);
    REQUIRE(core.output_state() == OutputState::Stopped);
    REQUIRE(always_matched);
}
