// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/safety-stage/spec.md: the output safety stage.

#include <ks/core/realtime.h>
#include <ks/engine/block.h>
#include <ks/engine/output_controller.h>
#include <ks/engine/paced_backend.h>
#include <ks/engine/render_core.h>
#include <ks/engine/safety.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using ks::engine::Block;
using ks::engine::DriverId;
using ks::engine::Graph;
using ks::engine::OutputConfig;
using ks::engine::OutputController;
using ks::engine::OutputState;
using ks::engine::PacedConfig;
using ks::engine::PacedDeviceSource;
using ks::engine::RenderCore;
using ks::engine::SafetyConfig;
using ks::engine::SimulatedDevice;
using ks::engine::test::default_ceiling;
using ks::engine::test::FunctionGraph;
using ks::engine::test::kFadeFrames;
using ks::engine::test::kRate;
using ks::engine::test::near;
using ks::engine::test::render;

using Channels = std::vector<std::vector<float>>;
using std::chrono::milliseconds;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

/// Describes the first sample in frames [first, last) of every channel that
/// isn't within 10⁻⁶ of expected(frame), or returns empty.
template <typename Expected>
std::string first_mismatch(const Channels& output, std::size_t first, std::size_t last,
                           Expected expected) {
    for (std::size_t c = 0; c < output.size(); ++c) {
        for (std::size_t f = first; f < last; ++f) {
            const double want = expected(f);
            if (!near(output.at(c).at(f), want)) {
                return "frame " + std::to_string(f) + ", channel " + std::to_string(c) + ": " +
                       std::to_string(output.at(c).at(f)) + " instead of " + std::to_string(want);
            }
        }
    }
    return {};
}

/// The fade-in gain j frames after it started.
double fade_in(std::size_t j) {
    return j >= kFadeFrames ? 1.0 : static_cast<double>(j) / static_cast<double>(kFadeFrames);
}

auto constant(float value) {
    return [value](std::uint64_t /*frame*/, std::size_t /*channel*/) { return value; };
}

} // namespace

TEST_CASE("Safety: fades in after a start and after a handover", "[safety]") {
    FunctionGraph graph{constant(0.5F)};
    RenderCore core{graph};

    const Channels started = render(core, 1000);
    CAPTURE(first_mismatch(started, 0, 1000, [](std::size_t f) { return 0.5 * fade_in(f); }));
    REQUIRE(
        first_mismatch(started, 0, 1000, [](std::size_t f) { return 0.5 * fade_in(f); }).empty());
    for (const std::vector<float>& channel : started) {
        for (std::size_t f = kFadeFrames; f < 1000; ++f) {
            REQUIRE(channel.at(f) == 0.5F);
        }
    }

    constexpr DriverId kNext{1};
    core.activate(kNext);
    const Channels handed_over = render(core, 1000, kNext);
    CAPTURE(first_mismatch(handed_over, 0, 1000, [](std::size_t f) { return 0.5 * fade_in(f); }));
    REQUIRE(first_mismatch(handed_over, 0, 1000, [](std::size_t f) {
                return 0.5 * fade_in(f);
            }).empty());
}

TEST_CASE("Safety: NaN and Inf become silence, then fade back in", "[safety]") {
    FunctionGraph graph{[](std::uint64_t frame, std::size_t channel) {
        if (frame == 1000 && channel == 0) {
            return kNaN;
        }
        if (frame == 1500 && channel == 1) {
            return kInf;
        }
        if (frame == 2000) {
            return -kInf;
        }
        return 0.5F;
    }};
    RenderCore core{graph};
    const Channels output = render(core, 3000);

    for (const std::size_t bad : {std::size_t{1000}, std::size_t{1500}, std::size_t{2000}}) {
        CAPTURE(bad);
        for (const std::vector<float>& channel : output) {
            REQUIRE(channel.at(bad) == 0.0F);
        }
        const std::string mismatch =
            first_mismatch(output, bad + 1, bad + 1 + kFadeFrames,
                           [bad](std::size_t f) { return 0.5 * fade_in(f - bad); });
        CAPTURE(mismatch);
        REQUIRE(mismatch.empty());
    }
    for (const std::vector<float>& channel : output) {
        for (const float sample : channel) {
            REQUIRE(std::isfinite(sample));
        }
    }
    REQUIRE(core.non_finite_samples() == 4);
}

TEST_CASE("Safety: the limiter holds the ceiling and recovers", "[safety]") {
    constexpr std::size_t kBurstStart = kRate / 5;                // 0.2 s
    constexpr std::size_t kBurstEnd = kBurstStart + (kRate / 10); // 0.1 s of burst
    constexpr std::size_t kTotal = kBurstEnd + kRate + 1000;      // 1 s after, and a little
    const double ceiling = default_ceiling();

    FunctionGraph graph{[](std::uint64_t frame, std::size_t /*channel*/) {
        return frame >= kBurstStart && frame < kBurstEnd ? 2.0F : 0.5F;
    }};
    RenderCore core{graph};
    const Channels output = render(core, kTotal);

    for (const std::vector<float>& channel : output) {
        for (std::size_t f = 0; f < kTotal; ++f) {
            CAPTURE(f);
            REQUIRE(std::abs(static_cast<double>(channel.at(f))) <=
                    ceiling + ks::engine::test::kTolerance);
        }
    }
    const std::string burst =
        first_mismatch(output, kBurstStart, kBurstEnd, [ceiling](std::size_t) { return ceiling; });
    CAPTURE(burst);
    REQUIRE(burst.empty());

    // One release time constant after the burst: 1 − (1 − ceiling/2) / e.
    const double recovering = static_cast<double>(output.at(0).at(kBurstEnd + (kRate / 10))) / 0.5;
    CAPTURE(recovering);
    REQUIRE(recovering >= 0.78);
    REQUIRE(recovering <= 0.81);

    // A second after the burst, fully recovered.
    for (const std::vector<float>& channel : output) {
        REQUIRE(channel.at(kBurstEnd + kRate) == 0.5F);
    }

    REQUIRE(core.limited_frames() > kBurstEnd - kBurstStart);
    REQUIRE(core.limited_frames() < kTotal);

    SECTION("huge values") {
        FunctionGraph huge{[](std::uint64_t frame, std::size_t /*channel*/) {
            return frame % 2 == 0 ? 1e30F : -1e30F;
        }};
        RenderCore huge_core{huge};
        const Channels clamped = render(huge_core, 1000);
        const std::string mismatch = first_mismatch(
            clamped, 1, 1000, [ceiling](std::size_t f) { return f % 2 == 0 ? ceiling : -ceiling; });
        CAPTURE(mismatch);
        REQUIRE(mismatch.empty());
    }

    SECTION("another ceiling") {
        const double minus_six = std::pow(10.0, -6.0 / 20.0);
        FunctionGraph loud{constant(2.0F)};
        RenderCore quieter{loud, SafetyConfig{.ceiling_dbfs = -6.0}};
        const Channels capped = render(quieter, 2000);
        for (const std::vector<float>& channel : capped) {
            for (const float sample : channel) {
                REQUIRE(std::abs(static_cast<double>(sample)) <=
                        minus_six + ks::engine::test::kTolerance);
            }
        }
        const std::string mismatch = first_mismatch(capped, kFadeFrames, 2000,
                                                    [minus_six](std::size_t) { return minus_six; });
        CAPTURE(mismatch);
        REQUIRE(mismatch.empty());
    }
}

TEST_CASE("Safety: each stereo pair has its own limiter", "[safety]") {
    constexpr std::size_t kTotal = 4'000;
    const double ceiling = default_ceiling();
    const auto in_burst = [](std::uint64_t frame) { return frame >= 1'000 && frame < 2'000; };

    // A burst on channels 2 and 3 only.
    FunctionGraph graph{[in_burst](std::uint64_t frame, std::size_t channel) {
        return channel >= 2 && in_burst(frame) ? 2.0F : 0.5F;
    }};
    RenderCore core{graph};
    const Channels output = render(core, kTotal, ks::engine::kDefaultDriver, 64, 4);

    for (std::size_t c = 0; c < 2; ++c) {
        for (std::size_t f = kFadeFrames; f < kTotal; ++f) {
            CAPTURE(c, f);
            REQUIRE(output.at(c).at(f) == 0.5F);
        }
    }
    const Channels limited{output.at(2), output.at(3)};
    const std::string held =
        first_mismatch(limited, 1'000, 2'000, [ceiling](std::size_t) { return ceiling; });
    CAPTURE(held);
    REQUIRE(held.empty());
    REQUIRE(core.limited_frames() > 1'000);

    // The same burst on both pairs counts the same frames: each frame once.
    FunctionGraph everywhere{[in_burst](std::uint64_t frame, std::size_t /*channel*/) {
        return in_burst(frame) ? 2.0F : 0.5F;
    }};
    RenderCore both{everywhere};
    static_cast<void>(render(both, kTotal, ks::engine::kDefaultDriver, 64, 4));
    REQUIRE(both.limited_frames() == core.limited_frames());
}

namespace {

/// Checks flush-to-zero from inside the graph, and writes a denormal.
class DenormalGraph final : public Graph {
public:
    void process(const Block& block) noexcept KS_NONBLOCKING override {
        const volatile float tiny = FLT_MIN;
        const volatile float half = 0.5F;
        const float product = tiny * half;
        if (product != 0.0F) {
            saw_denormal_ = true;
        }
        for (const std::span<float> channel : block.channels) {
            for (float& sample : channel.first(block.frames)) {
                sample = FLT_MIN / 4.0F; // a denormal constant
            }
        }
    }

    /// True if any multiplication inside the graph produced a denormal.
    [[nodiscard]] bool saw_denormal() const noexcept { return saw_denormal_; }

private:
    bool saw_denormal_ = false;
};

} // namespace

TEST_CASE("Safety: denormals are flushed while rendering", "[safety]") {
    STATIC_REQUIRE(FLT_MIN / 4.0F != 0.0F); // it really is a denormal, not zero

    DenormalGraph graph;
    RenderCore core{graph};
    const Channels output = render(core, 1000);

    REQUIRE_FALSE(graph.saw_denormal());
    for (const std::vector<float>& channel : output) {
        for (std::size_t f = kFadeFrames; f < 1000; ++f) {
            REQUIRE(channel.at(f) == 0.0F);
        }
    }

    // The calling thread's floating-point mode is back to normal.
    const volatile float tiny = FLT_MIN;
    const volatile float half = 0.5F;
    const float product = tiny * half;
    REQUIRE(product != 0.0F);
}

TEST_CASE("Safety: muting fades out, and unmuting fades back in", "[safety]") {
    FunctionGraph graph{constant(0.5F)};
    RenderCore core{graph};
    static_cast<void>(render(core, 1000));
    REQUIRE_FALSE(core.silenced());

    core.set_muted(true);
    const Channels fading = render(core, 1000);
    const std::string out = first_mismatch(fading, 0, 1000, [](std::size_t f) {
        return f >= kFadeFrames
                   ? 0.0
                   : 0.5 * static_cast<double>(kFadeFrames - f) / static_cast<double>(kFadeFrames);
    });
    CAPTURE(out);
    REQUIRE(out.empty());
    for (const std::vector<float>& channel : fading) {
        for (std::size_t f = kFadeFrames; f < 1000; ++f) {
            REQUIRE(channel.at(f) == 0.0F);
        }
    }
    REQUIRE(core.silenced());

    core.set_muted(false);
    const Channels returning = render(core, 1000);
    const std::string in =
        first_mismatch(returning, 0, 1000, [](std::size_t f) { return 0.5 * fade_in(f); });
    CAPTURE(in);
    REQUIRE(in.empty());
    REQUIRE_FALSE(core.silenced());

    SECTION("muted from the start") {
        FunctionGraph quiet_graph{constant(0.5F)};
        RenderCore quiet{quiet_graph};
        quiet.set_muted(true);
        const Channels silent = render(quiet, 1000);
        for (const std::vector<float>& channel : silent) {
            for (const float sample : channel) {
                REQUIRE(sample == 0.0F);
            }
        }
        REQUIRE(quiet.silenced());
    }
}

TEST_CASE("Safety: output doesn't depend on block size", "[safety]") {
    constexpr std::size_t kFrames = 12'000;
    constexpr std::array<std::size_t, 5> kBlockSizes{1, 37, 128, 512, 4096};
    const auto loud = [](std::uint64_t frame, std::size_t channel) {
        if ((frame == 3000 && channel == 0) || (frame == 7000 && channel == 1)) {
            return kNaN;
        }
        const double phase = (2.0 * std::numbers::pi * 440.0 * static_cast<double>(frame) / kRate) +
                             static_cast<double>(channel);
        return static_cast<float>(1.5 * std::sin(phase));
    };

    Channels reference;
    std::uint64_t reference_non_finite = 0;
    std::uint64_t reference_limited = 0;
    for (const std::size_t block_frames : kBlockSizes) {
        CAPTURE(block_frames);
        FunctionGraph graph{loud};
        RenderCore core{graph};
        const Channels output = render(core, kFrames, ks::engine::kDefaultDriver, block_frames);
        if (reference.empty()) {
            reference = output;
            reference_non_finite = core.non_finite_samples();
            reference_limited = core.limited_frames();
            REQUIRE(reference_non_finite == 2);
            REQUIRE(reference_limited > 0);
            continue;
        }
        for (std::size_t c = 0; c < output.size(); ++c) {
            for (std::size_t f = 0; f < kFrames; ++f) {
                if (std::bit_cast<std::uint32_t>(output.at(c).at(f)) !=
                    std::bit_cast<std::uint32_t>(reference.at(c).at(f))) {
                    FAIL("frame " << f << ", channel " << c << " differs");
                }
            }
        }
        REQUIRE(core.non_finite_samples() == reference_non_finite);
        REQUIRE(core.limited_frames() == reference_limited);
    }
}

TEST_CASE("Safety: the transparent configuration still runs", "[safety]") {
    const SafetyConfig transparent{.fade = milliseconds{0}, .ceiling_dbfs = 0.0};
    const auto value = [](std::uint64_t frame, std::size_t channel) {
        if (frame == 500 && channel == 0) {
            return kNaN;
        }
        if (frame == 700) {
            return FLT_MIN / 4.0F; // denormal
        }
        if (frame == 1000) {
            return channel == 0 ? 1.5F : -1.5F; // last: the limiter then takes time to release
        }
        // In range, and different on every frame.
        return static_cast<float>(
            std::sin(static_cast<double>(frame) + (2.0 * static_cast<double>(channel))) * 0.99);
    };
    FunctionGraph graph{value};
    RenderCore core{graph, transparent};
    const Channels output = render(core, 1001);

    for (std::size_t c = 0; c < output.size(); ++c) {
        for (std::size_t f = 0; f < 1000; ++f) {
            CAPTURE(c, f);
            if (f == 500 && c == 0) {
                REQUIRE(output.at(c).at(f) == 0.0F); // NaN
            } else if (f == 700) {
                REQUIRE(output.at(c).at(f) == 0.0F); // denormal, flushed
            } else if (f != 500) {
                REQUIRE(std::bit_cast<std::uint32_t>(output.at(c).at(f)) ==
                        std::bit_cast<std::uint32_t>(value(f, c)));
            }
        }
    }
    REQUIRE(near(output.at(0).at(1000), 1.0));
    REQUIRE(near(output.at(1).at(1000), -1.0));
    REQUIRE(core.non_finite_samples() == 1);
}

TEST_CASE("Safety: stopping the output fades out first", "[safety]") {
    FunctionGraph graph{constant(0.5F)};
    RenderCore core{graph};
    SimulatedDevice device;
    PacedDeviceSource source{
        device, PacedConfig{.sample_rate = kRate, .channels = 2, .period_frames = 256}};
    OutputController controller{core, source, OutputConfig{}};

    controller.start();
    REQUIRE(controller.state() == OutputState::Playing);
    const auto until = std::chrono::steady_clock::now() + milliseconds{100};
    while (std::chrono::steady_clock::now() < until) {
        controller.poll();
        std::this_thread::sleep_for(milliseconds{5});
    }

    const std::uint64_t before_stop = core.frames_rendered();
    controller.stop();
    REQUIRE(controller.state() == OutputState::Stopped);
    REQUIRE(core.silenced());
    REQUIRE(core.frames_rendered() - before_stop >= kFadeFrames);

    controller.start();
    REQUIRE(controller.state() == OutputState::Playing);
    const std::uint64_t restarted = core.frames_rendered();
    const auto deadline = std::chrono::steady_clock::now() + milliseconds{1000};
    while (core.frames_rendered() == restarted && std::chrono::steady_clock::now() < deadline) {
        controller.poll();
        std::this_thread::sleep_for(milliseconds{1});
    }
    REQUIRE(core.frames_rendered() > restarted);
    REQUIRE_FALSE(core.silenced());
    controller.stop();
}
