// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/safety.h>

#include <ks/core/realtime.h>
#include <ks/engine/block.h>
#include <ks/engine/offline.h>
#include <ks/engine/render_core.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

using ks::engine::Block;
using ks::engine::Graph;
using ks::engine::OfflineConfig;
using ks::engine::render_offline;
using ks::engine::RenderCore;
using ks::engine::SafetyConfig;
using ks::engine::SafetyStage;
using std::chrono::milliseconds;

constexpr std::uint32_t kRate = 48'000;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
const SafetyConfig kNoFade{.fade = milliseconds{0}, .ceiling_dbfs = 0.0};

/// Two channels, each filled with `fill`.
struct Stereo {
    Stereo(std::size_t frames, float fill) : left(frames, fill), right(frames, fill) {}

    std::vector<float> left;
    std::vector<float> right;
};

/// Runs `stage` over all of `frames`, in place, as one block.
void run(SafetyStage& stage, Stereo& frames, bool muted = false,
         std::uint32_t sample_rate = kRate) {
    const std::array<std::span<float>, 2> channels{frames.left, frames.right};
    stage.process(Block{.channels = channels,
                        .frames = frames.left.size(),
                        .sample_rate = sample_rate,
                        .host_time = {},
                        .resumed = false},
                  muted);
}

/// True if `actual` is within 10⁻⁶ of `expected`.
bool near(float actual, double expected) {
    return std::abs(static_cast<double>(actual) - expected) <= 1e-6;
}

/// Writes 0.5 to every sample.
class HalfGraph final : public Graph {
public:
    void process(const Block& block) noexcept KS_NONBLOCKING override {
        for (const std::span<float> channel : block.channels) {
            std::ranges::fill(channel.first(block.frames), 0.5F);
        }
    }
};

/// Renders `frames` frames of two channels through `core`, offline.
void render(RenderCore& core, std::size_t frames) {
    std::vector<float> left(frames);
    std::vector<float> right(frames);
    const std::array<std::span<float>, 2> channels{left, right};
    const OfflineConfig config{.sample_rate = kRate,
                               .block_frames = 64,
                               .start_time = {},
                               .driver = ks::engine::kDefaultDriver};
    static_cast<void>(render_offline(core, config, channels));
}

} // namespace

TEST_CASE("Safety stage: with no fade, a non-finite sample still silences its frame",
          "[engine][safety]") {
    SafetyStage stage{kNoFade};
    Stereo frames{4, 0.5F};
    frames.left.at(1) = kNaN;
    run(stage, frames);

    // The other channel is silent too, and the next frame is back at once.
    REQUIRE(frames.left == std::vector{0.5F, 0.0F, 0.5F, 0.5F});
    REQUIRE(frames.right == std::vector{0.5F, 0.0F, 0.5F, 0.5F});
    REQUIRE(stage.non_finite_samples() == 1);
}

TEST_CASE("Safety stage: with no fade, muting and unmuting are immediate", "[engine][safety]") {
    SafetyStage stage{kNoFade};
    Stereo muted{4, 0.5F};
    run(stage, muted, true);
    REQUIRE(muted.left == std::vector(4, 0.0F));
    REQUIRE(muted.right == std::vector(4, 0.0F));
    REQUIRE(stage.silenced());

    Stereo unmuted{4, 0.5F};
    run(stage, unmuted, false);
    REQUIRE(unmuted.left == std::vector(4, 0.5F));
    REQUIRE(unmuted.right == std::vector(4, 0.5F));
    REQUIRE_FALSE(stage.silenced());
}

TEST_CASE("Safety stage: a mute midway through a fade in turns it around", "[engine][safety]") {
    SafetyStage stage{SafetyConfig{}};
    Stereo fading_in{100, 0.5F};
    run(stage, fading_in);

    Stereo fading_out{3, 0.5F};
    run(stage, fading_out, true);
    REQUIRE(near(fading_out.left.at(0), 0.5 * 100.0 / 480.0));
    REQUIRE(near(fading_out.left.at(1), 0.5 * 99.0 / 480.0));
    REQUIRE(near(fading_out.left.at(2), 0.5 * 98.0 / 480.0));
}

TEST_CASE("Safety stage: a ceiling above 0 dBFS is treated as 0 dBFS", "[engine][safety]") {
    for (const double ceiling : {6.0, std::numeric_limits<double>::quiet_NaN()}) {
        CAPTURE(ceiling);
        SafetyStage stage{SafetyConfig{.fade = milliseconds{0}, .ceiling_dbfs = ceiling}};
        Stereo frames{4, 1.5F};
        run(stage, frames);
        REQUIRE(frames.left == std::vector(4, 1.0F));
        REQUIRE(frames.right == std::vector(4, 1.0F));
    }
}

TEST_CASE("Safety stage: the limiter stays on while the output is just over the ceiling",
          "[engine][safety]") {
    // The gain this needs is within 10⁻⁴ of 1, but it mustn't snap to 1
    // until the target is 1.
    SafetyStage stage{kNoFade};
    Stereo over{1000, 1.00005F};
    run(stage, over);
    REQUIRE(stage.limited_frames() == 1000);
    REQUIRE(std::ranges::all_of(over.left, [](float sample) { return sample <= 1.0F; }));

    // Once under the ceiling, it's that close, so it snaps at once.
    Stereo under{2, 0.5F};
    run(stage, under);
    REQUIRE(under.left == std::vector(2, 0.5F));
    REQUIRE(stage.limited_frames() == 1000);
}

TEST_CASE("Safety stage: a release of 0 recovers at once", "[engine][safety]") {
    SafetyStage stage{SafetyConfig{.fade = milliseconds{0}, .release = milliseconds{0}}};
    Stereo frames{3, 0.5F};
    frames.left.at(0) = 2.0F;
    run(stage, frames);
    REQUIRE(near(frames.left.at(0), std::pow(10.0, -1.0 / 20.0)));
    REQUIRE(frames.left.at(1) == 0.5F);
    REQUIRE(stage.limited_frames() == 1);
}

TEST_CASE("Safety stage: a sample rate change keeps the fade position within the fade",
          "[engine][safety]") {
    SafetyStage stage{SafetyConfig{}};
    Stereo faded_in{480, 0.5F};
    run(stage, faded_in, false, kRate);

    // At 24 kHz the fade is 240 frames, so the position, 480, becomes 240:
    // still the full gain.
    Stereo slower{1, 0.5F};
    run(stage, slower, false, kRate / 2);
    REQUIRE(slower.left.at(0) == 0.5F);

    // At 96 kHz the fade is 960 frames, and the position stays 240.
    Stereo faster{2, 0.5F};
    run(stage, faster, false, kRate * 2);
    REQUIRE(near(faster.left.at(0), 0.5 * 240.0 / 960.0));
    REQUIRE(near(faster.left.at(1), 0.5 * 241.0 / 960.0));
}

TEST_CASE("Render core: silenced() is false as soon as it's unmuted", "[engine][safety]") {
    HalfGraph graph;
    RenderCore core{graph};
    core.set_muted(true);
    render(core, 1000);
    REQUIRE(core.silenced());

    // Before another block renders.
    core.set_muted(false);
    REQUIRE_FALSE(core.silenced());

    // Muted again, the last block still ended in silence.
    core.set_muted(true);
    REQUIRE(core.silenced());
}
