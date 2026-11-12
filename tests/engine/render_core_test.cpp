// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/render_core.h>

#include <ks/core/realtime.h>
#include <ks/engine/block.h>
#include <ks/engine/interleave.h>
#include <ks/engine/offline.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

using ks::engine::Block;
using ks::engine::frames_to_duration;
using ks::engine::Graph;
using ks::engine::kMaxBlockFrames;
using ks::engine::OfflineConfig;
using ks::engine::render_interleaved;
using ks::engine::render_offline;
using ks::engine::RenderCore;

/// Writes 1 to every sample, and records the size of each block, up to 16.
class CountingGraph final : public Graph {
public:
    void process(const Block& block) noexcept KS_NONBLOCKING override {
        for (const std::span<float> channel : block.channels) {
            for (float& sample : channel.first(block.frames)) {
                sample = 1.0F;
            }
        }
        if (count_ < sizes_.size()) {
            std::span{sizes_}[count_] = block.frames;
        }
        ++count_;
    }

    [[nodiscard]] std::vector<std::size_t> sizes() const {
        const std::span<const std::size_t> recorded = std::span{sizes_}.first(count_);
        return {recorded.begin(), recorded.end()};
    }

    [[nodiscard]] std::size_t count() const noexcept { return count_; }

private:
    std::array<std::size_t, 16> sizes_{};
    std::size_t count_ = 0;
};

/// Records the output latency of each block, up to 16.
class LatencyGraph final : public Graph {
public:
    void process(const Block& block) noexcept KS_NONBLOCKING override {
        if (count_ < latencies_.size()) {
            std::span{latencies_}[count_] = block.output_latency;
        }
        ++count_;
    }

    [[nodiscard]] std::vector<std::chrono::nanoseconds> latencies() const {
        const std::span<const std::chrono::nanoseconds> recorded =
            std::span{latencies_}.first(count_);
        return {recorded.begin(), recorded.end()};
    }

private:
    std::array<std::chrono::nanoseconds, 16> latencies_{};
    std::size_t count_ = 0;
};

/// While rendering, renders a whole device callback through the same core,
/// as a second driver would if its callback arrived mid-render.
class NestedDeviceGraph final : public Graph {
public:
    NestedDeviceGraph(std::span<const std::span<float>> scratch, std::span<float> interleaved)
        : scratch_{scratch}, interleaved_{interleaved} {}

    void set_core(RenderCore& core) noexcept { core_ = &core; }

    void process(const Block& /*block*/) noexcept KS_NONBLOCKING override {
        nested_frames_ = render_interleaved(*core_, scratch_, interleaved_, 2, 48'000, {});
    }

    [[nodiscard]] std::size_t nested_frames() const noexcept { return nested_frames_; }

private:
    RenderCore* core_ = nullptr;
    std::span<const std::span<float>> scratch_;
    std::span<float> interleaved_;
    std::size_t nested_frames_ = 1;
};

} // namespace

TEST_CASE("frames_to_duration saturates instead of overflowing", "[engine]") {
    using std::chrono::nanoseconds;
    // Just below the limit: still exact.
    REQUIRE(frames_to_duration(9'223'372'035, 1) == nanoseconds{9'223'372'035'000'000'000});
    REQUIRE(frames_to_duration(std::numeric_limits<std::uint64_t>::max(), 1) == nanoseconds::max());
    REQUIRE(frames_to_duration(std::numeric_limits<std::uint64_t>::max(), 48'000) ==
            nanoseconds::max());
}

TEST_CASE("Offline rendering keeps the configured block boundaries", "[engine]") {
    CountingGraph graph;
    RenderCore core{graph};
    std::vector<float> samples(1400);
    const std::array<std::span<float>, 1> output{std::span{samples}};
    const OfflineConfig config{.sample_rate = 48'000, .block_frames = 700, .start_time = {}};

    REQUIRE(render_offline(core, config, output) == 1400);
    REQUIRE(graph.sizes() == std::vector<std::size_t>{kMaxBlockFrames, 188, kMaxBlockFrames, 188});
}

TEST_CASE("Offline rendering of no frames renders nothing", "[engine]") {
    CountingGraph graph;
    RenderCore core{graph};
    const std::array<std::span<float>, 2> output{};
    const OfflineConfig config{.sample_rate = 48'000, .block_frames = 64, .start_time = {}};

    REQUIRE(render_offline(core, config, output) == 0);
    REQUIRE(graph.count() == 0);
}

TEST_CASE("A device callback that arrives mid-render plays silence", "[engine]") {
    std::vector<float> scratch_storage(2 * kMaxBlockFrames, 5.0F);
    const std::array<std::span<float>, 2> scratch{std::span{scratch_storage}.first(kMaxBlockFrames),
                                                  std::span{scratch_storage}.last(kMaxBlockFrames)};
    std::vector<float> device(std::size_t{2} * 1000, 5.0F);

    NestedDeviceGraph graph{scratch, device};
    RenderCore core{graph};
    graph.set_core(core);

    std::vector<float> samples(64);
    const std::array<std::span<float>, 1> channels{std::span{samples}};
    REQUIRE(core.try_render(Block{
                .channels = channels, .frames = 64, .sample_rate = 48'000, .host_time = {}}) ==
            ks::engine::RenderResult::Rendered);

    REQUIRE(graph.nested_frames() == 0);
    for (const float sample : device) {
        REQUIRE(sample == 0.0F);
    }
    REQUIRE(core.frames_rendered() == 64);
}

TEST_CASE("A device callback of no frames renders nothing", "[engine]") {
    CountingGraph graph;
    RenderCore core{graph};
    std::vector<float> scratch_storage(kMaxBlockFrames);
    const std::array<std::span<float>, 1> scratch{std::span{scratch_storage}};

    REQUIRE(render_interleaved(core, scratch, {}, 1, 48'000, {}) == 0);
    REQUIRE(graph.count() == 0);
}

TEST_CASE("A device callback's output latency reaches every block it splits into", "[engine]") {
    LatencyGraph graph;
    RenderCore core{graph};
    std::vector<float> scratch_storage(kMaxBlockFrames);
    const std::array<std::span<float>, 1> scratch{std::span{scratch_storage}};
    // Three full blocks and a partial one.
    std::vector<float> device((3 * kMaxBlockFrames) + 10);
    const std::chrono::nanoseconds latency{7'250'000};

    REQUIRE(render_interleaved(core, scratch, device, 1, 48'000, {}, {}, latency) == device.size());
    REQUIRE(graph.latencies() == std::vector<std::chrono::nanoseconds>(4, latency));

    LatencyGraph defaulted;
    RenderCore default_core{defaulted};
    REQUIRE(render_interleaved(default_core, scratch, device, 1, 48'000, {}) == device.size());
    REQUIRE(defaulted.latencies() == std::vector<std::chrono::nanoseconds>(4));
}

TEST_CASE("Interleaving no channels succeeds and writes nothing", "[engine]") {
    std::vector<float> interleaved(4, 5.0F);
    REQUIRE(ks::engine::interleave({}, 10, interleaved));
    REQUIRE(interleaved == std::vector<float>(4, 5.0F));
}
