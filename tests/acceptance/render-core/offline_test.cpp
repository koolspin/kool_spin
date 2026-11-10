// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/render-core/spec.md: the offline driver.

#include <ks/engine/offline.h>

#include <ks/engine/block.h>
#include <ks/engine/render_core.h>
#include <ks/engine/safety.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

using ks::engine::frames_to_duration;
using ks::engine::kMaxBlockFrames;
using ks::engine::kMaxOutputChannels;
using ks::engine::OfflineConfig;
using ks::engine::render_offline;
using ks::engine::RenderCore;
using ks::engine::test::BlockRecord;
using ks::engine::test::Planar;
using ks::engine::test::signal;
using ks::engine::test::SignalGraph;

using TimePoint = std::chrono::steady_clock::time_point;

/// The first frame and channel whose sample isn't signal(frame, channel), as
/// text, or empty if every sample matches.
std::string first_mismatch(const Planar& output, std::size_t channels, std::size_t frames) {
    for (std::size_t c = 0; c < channels; ++c) {
        const std::span<const float> channel = output.channel(c);
        for (std::size_t f = 0; f < frames; ++f) {
            if (channel[f] != signal(f, c)) {
                return "frame " + std::to_string(f) + ", channel " + std::to_string(c);
            }
        }
    }
    return {};
}

bool same_bits(std::span<const float> a, std::span<const float> b) {
    return std::ranges::equal(a, b, [](float x, float y) {
        return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y);
    });
}

/// Passes in-range samples through unchanged, so the samples can be checked
/// exactly. The safety stage still runs (specs/safety-stage/spec.md, req. 18).
const ks::engine::SafetyConfig kTransparent{.fade = std::chrono::milliseconds{0},
                                            .ceiling_dbfs = 0.0};

} // namespace

TEST_CASE("Offline: every frame, in blocks of at most kMaxBlockFrames", "[offline]") {
    constexpr std::size_t kFrames = 10'000;
    constexpr std::size_t kChannels = 3;
    constexpr std::uint32_t kRate = 44'100;
    const TimePoint start{std::chrono::seconds{1000}};

    SignalGraph graph{1000};
    RenderCore core{graph, kTransparent};
    const Planar output{kChannels, kFrames, 5.0F};
    const OfflineConfig config{.sample_rate = kRate, .block_frames = 4096, .start_time = start};

    REQUIRE(render_offline(core, config, output.channels()) == kFrames);
    REQUIRE(core.frames_rendered() == kFrames);

    const std::span<const BlockRecord> records = graph.records();
    REQUIRE(records.size() == graph.calls());
    std::uint64_t offset = 0;
    for (const BlockRecord& record : records) {
        CAPTURE(offset, record.frames);
        REQUIRE(record.frames >= 1);
        REQUIRE(record.frames <= kMaxBlockFrames);
        REQUIRE(record.channels == kChannels);
        REQUIRE(record.sample_rate == kRate);
        REQUIRE(record.host_time == start + frames_to_duration(offset, kRate));
        offset += record.frames;
    }
    REQUIRE(offset == kFrames);

    // Every frame landed in its place.
    REQUIRE(first_mismatch(output, kChannels, kFrames).empty());
}

TEST_CASE("Offline: output doesn't depend on block size", "[offline]") {
    constexpr std::size_t kFrames = 5'000;
    constexpr std::size_t kChannels = 2;
    constexpr std::uint32_t kRate = 48'000;
    constexpr std::array<std::size_t, 5> kBlockSizes{1, 37, 128, 512, 4096};

    std::vector<std::vector<float>> reference;
    for (const std::size_t block_frames : kBlockSizes) {
        CAPTURE(block_frames);
        SignalGraph graph{0};
        RenderCore core{graph, kTransparent};
        const Planar output{kChannels, kFrames, 5.0F};
        const OfflineConfig config{
            .sample_rate = kRate, .block_frames = block_frames, .start_time = {}};
        REQUIRE(render_offline(core, config, output.channels()) == kFrames);

        if (reference.empty()) {
            for (std::size_t c = 0; c < kChannels; ++c) {
                const std::span<const float> channel = output.channel(c);
                reference.emplace_back(channel.begin(), channel.end());
            }
            REQUIRE(first_mismatch(output, kChannels, kFrames).empty());
            continue;
        }
        for (std::size_t c = 0; c < kChannels; ++c) {
            CAPTURE(c);
            REQUIRE(same_bits(output.channel(c), reference.at(c)));
        }
    }
}

TEST_CASE("Offline: invalid configurations render nothing", "[offline]") {
    constexpr std::uint32_t kRate = 48'000;
    struct Case {
        std::string name;
        std::vector<std::size_t> lengths; // one per channel
        std::size_t block_frames;
        std::uint32_t sample_rate;
    };
    const std::vector<Case> cases{
        {.name = "no channels", .lengths = {}, .block_frames = 64, .sample_rate = kRate},
        {.name = "more than kMaxOutputChannels",
         .lengths = std::vector<std::size_t>(kMaxOutputChannels + 1, 256),
         .block_frames = 64,
         .sample_rate = kRate},
        {.name = "channels of different lengths",
         .lengths = {256, 255},
         .block_frames = 64,
         .sample_rate = kRate},
        {.name = "a block size of 0",
         .lengths = {256, 256},
         .block_frames = 0,
         .sample_rate = kRate},
        {.name = "a sample rate of 0", .lengths = {256, 256}, .block_frames = 64, .sample_rate = 0},
    };

    for (const Case& c : cases) {
        CAPTURE(c.name);
        SignalGraph graph{0};
        RenderCore core{graph};
        const Planar output{c.lengths, 5.0F};
        const OfflineConfig config{
            .sample_rate = c.sample_rate, .block_frames = c.block_frames, .start_time = {}};
        REQUIRE(render_offline(core, config, output.channels()) == 0);
        REQUIRE(output.all_equal(5.0F));
        REQUIRE(graph.calls() == 0);
        REQUIRE(core.frames_rendered() == 0);
    }

    // The control: a valid configuration renders.
    SignalGraph graph{0};
    RenderCore core{graph, kTransparent};
    const Planar output{{256, 256}, 5.0F};
    const OfflineConfig config{.sample_rate = kRate, .block_frames = 64, .start_time = {}};
    REQUIRE(render_offline(core, config, output.channels()) == 256);
    REQUIRE(first_mismatch(output, 2, 256).empty());
}
