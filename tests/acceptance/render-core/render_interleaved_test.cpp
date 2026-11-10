// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/render-core/spec.md: a device driver's work for
// one callback.

#include <ks/engine/block.h>
#include <ks/engine/interleave.h>
#include <ks/engine/render_core.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
using ks::engine::render_interleaved;
using ks::engine::RenderCore;
using ks::engine::test::BlockRecord;
using ks::engine::test::Planar;
using ks::engine::test::signal;
using ks::engine::test::SignalGraph;

using TimePoint = std::chrono::steady_clock::time_point;

} // namespace

TEST_CASE("Device render: blocks, host times, and interleaving", "[device-render]") {
    constexpr std::size_t kChannels = 3;
    constexpr std::size_t kFrames = 2000;
    constexpr std::uint32_t kRate = 44'100;
    const TimePoint host_time{std::chrono::seconds{50}};

    SignalGraph graph{100};
    RenderCore core{graph};
    // More scratch buffers than channels: the first kChannels are used.
    const Planar scratch{kMaxOutputChannels, kMaxBlockFrames, 0.0F};
    std::vector<float> interleaved(kFrames * kChannels, 5.0F);

    REQUIRE(render_interleaved(core, scratch.channels(), interleaved, kChannels, kRate,
                               host_time) == kFrames);
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
        REQUIRE(record.host_time == host_time + frames_to_duration(offset, kRate));
        offset += record.frames;
    }
    REQUIRE(offset == kFrames);

    // Every sample landed in its place.
    std::string mismatch;
    for (std::size_t f = 0; f < kFrames && mismatch.empty(); ++f) {
        for (std::size_t c = 0; c < kChannels; ++c) {
            if (interleaved.at((f * kChannels) + c) != signal(f, c)) {
                mismatch = "frame " + std::to_string(f) + ", channel " + std::to_string(c);
                break;
            }
        }
    }
    CAPTURE(mismatch);
    REQUIRE(mismatch.empty());

    // Inconsistent arguments: silence, and nothing rendered.
    struct Case {
        std::string name;
        std::vector<std::size_t> scratch_lengths; // one per scratch buffer
        std::size_t channels;
        std::size_t samples; // the interleaved buffer's size
        std::uint32_t sample_rate;
    };
    const std::vector<Case> cases{
        {.name = "no channels",
         .scratch_lengths = {kMaxBlockFrames, kMaxBlockFrames},
         .channels = 0,
         .samples = 200,
         .sample_rate = kRate},
        {.name = "more than kMaxOutputChannels",
         .scratch_lengths = std::vector<std::size_t>(kMaxOutputChannels + 1, kMaxBlockFrames),
         .channels = kMaxOutputChannels + 1,
         .samples = (kMaxOutputChannels + 1) * 100,
         .sample_rate = kRate},
        {.name = "fewer scratch buffers than channels",
         .scratch_lengths = {kMaxBlockFrames, kMaxBlockFrames},
         .channels = 3,
         .samples = 300,
         .sample_rate = kRate},
        {.name = "a scratch buffer shorter than kMaxBlockFrames",
         .scratch_lengths = {kMaxBlockFrames, kMaxBlockFrames, kMaxBlockFrames - 1},
         .channels = 3,
         .samples = 300,
         .sample_rate = kRate},
        {.name = "a buffer size that isn't a multiple of the channel count",
         .scratch_lengths = {kMaxBlockFrames, kMaxBlockFrames, kMaxBlockFrames},
         .channels = 3,
         .samples = 301,
         .sample_rate = kRate},
        {.name = "a sample rate of 0",
         .scratch_lengths = {kMaxBlockFrames, kMaxBlockFrames},
         .channels = 2,
         .samples = 200,
         .sample_rate = 0},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        SignalGraph silent_graph{0};
        RenderCore silent_core{silent_graph};
        const Planar bad_scratch{c.scratch_lengths, 0.0F};
        std::vector<float> buffer(c.samples, 5.0F);
        REQUIRE(render_interleaved(silent_core, bad_scratch.channels(), buffer, c.channels,
                                   c.sample_rate, host_time) == 0);
        REQUIRE(std::ranges::all_of(buffer, [](float sample) { return sample == 0.0F; }));
        REQUIRE(silent_graph.calls() == 0);
        REQUIRE(silent_core.frames_rendered() == 0);
    }
}
