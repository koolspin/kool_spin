// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/mixer/spec.md: the mixer.

#include <ks/engine/mixer.h>

#include <ks/engine/interpolator.h>
#include <ks/engine/offline.h>
#include <ks/engine/render_core.h>
#include <ks/engine/safety.h>

#include "../deck/wav.h"
#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {

using ks::engine::BusMap;
using ks::engine::CrossfaderSide;
using ks::engine::Interpolator;
using ks::engine::Mixer;
using ks::engine::MixerSettings;
using ks::engine::test::deck_value;
using ks::engine::test::gain_of_db;
using ks::engine::test::last;
using ks::engine::test::MixerAction;
using ks::engine::test::MixerControl;
using ks::engine::test::near;
using ks::engine::test::Outputs;
using ks::engine::test::OwnedTrack;
using ks::engine::test::render;
using ks::engine::test::Rig;
using ks::engine::test::side_a;
using ks::engine::test::side_b;

/// Frames to render after a control, so ramps and smoothing have finished.
constexpr std::size_t kSettle = 2'000;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInfinity = std::numeric_limits<double>::infinity();

/// Checks the main bus, on channels 0 and 1, settled at `left` and -left.
void check_main(const Outputs& out, double left) {
    CAPTURE(last(out, 0), left);
    REQUIRE(near(last(out, 0), left));
    REQUIRE(near(last(out, 1), -left));
}

/// Checks the headphones, on channels 2 and 3, settled at `left` and -left.
void check_headphones(const Outputs& out, double left) {
    CAPTURE(last(out, 2), left);
    REQUIRE(near(last(out, 2), left));
    REQUIRE(near(last(out, 3), -left));
}

/// The crossfader gain of deck 0 (side A), deck 1 (side B), or a thru deck.
double crossfader_gain(std::size_t deck, double position, double transition) {
    if (deck == 0) {
        return side_a(position, transition);
    }
    if (deck == 1) {
        return side_b(position, transition);
    }
    return 1.0;
}

/// The decks' constants, as doubles.
double value(std::size_t deck) {
    return static_cast<double>(deck_value(deck));
}

} // namespace

TEST_CASE("Mixer: buses go to their channel pairs", "[mixer]") {
    SECTION("the defaults") {
        Rig rig;
        rig.play_all();
        const Outputs out = render(rig.mixer, kSettle);
        check_main(out, 0.5);
        // Nothing is cued, and the headphones hear only the cue bus.
        for (std::size_t c = 2; c < 4; ++c) {
            for (const float sample : out.at(c)) {
                REQUIRE(sample == 0.0F);
            }
        }
    }

    SECTION("a 2-channel block gets only the main bus") {
        Rig rig;
        rig.play_all();
        const Outputs out = render(rig.mixer, kSettle, 2);
        check_main(out, 0.5);
    }

    SECTION("swapped buses") {
        Rig rig{BusMap{.main = 2, .cue = 0}};
        rig.play_all();
        rig.mixer.set_cue_mix(1.0);
        rig.mixer.set_main_level(-6.0);
        const Outputs out = render(rig.mixer, kSettle);
        // The main output, at -6 dB, on channels 2 and 3...
        REQUIRE(near(last(out, 2), 0.5 * gain_of_db(-6.0)));
        REQUIRE(near(last(out, 3), -0.5 * gain_of_db(-6.0)));
        // ...and the headphones, hearing the main bus before the level, on 0 and 1.
        check_main(out, 0.5);
    }

    SECTION("invalid maps are refused") {
        const Interpolator interpolator;
        for (const BusMap map :
             {BusMap{.main = 1, .cue = 2}, BusMap{.main = 0, .cue = 3}, BusMap{.main = 2, .cue = 2},
              BusMap{.main = 8, .cue = 0}, BusMap{.main = 0, .cue = 8}}) {
            CAPTURE(map.main, map.cue);
            const Mixer mixer{interpolator, map};
            REQUIRE(mixer.settings().bus_map == BusMap{});
        }
        Mixer mixer{interpolator};
        mixer.set_bus_map(BusMap{.main = 4, .cue = 6});
        REQUIRE(mixer.settings().bus_map == BusMap{.main = 4, .cue = 6});
        mixer.set_bus_map(BusMap{.main = 1, .cue = 2});
        REQUIRE(mixer.settings().bus_map == BusMap{.main = 4, .cue = 6});
    }
}

TEST_CASE("Mixer: trim and fader", "[mixer]") {
    // Deck 0 alone plays.
    Rig rig;
    rig.play(0);

    SECTION("trim +6 dB") {
        rig.mixer.set_trim(0, 6.0);
        check_main(render(rig.mixer, kSettle), value(0) * gain_of_db(6.0));
    }
    SECTION("trim -inf") {
        rig.mixer.set_trim(0, -kInfinity);
        check_main(render(rig.mixer, kSettle), 0.0);
    }
    SECTION("fader 0.5") {
        rig.mixer.set_fader(0, 0.5);
        check_main(render(rig.mixer, kSettle), value(0) * 0.25);
    }
    SECTION("fader 0") {
        rig.mixer.set_fader(0, 0.0);
        check_main(render(rig.mixer, kSettle), 0.0);
    }
}

TEST_CASE("Mixer: crossfader curves", "[mixer]") {
    // Deck 0 on side A, deck 1 on side B, decks 2 and 3 thru. Each deck plays
    // alone, so its contribution is measured on its own.
    for (const double transition : {1.0, 0.1}) {
        for (const double position : {0.0, 0.25, 0.5, 0.75, 0.95, 1.0}) {
            for (std::size_t deck = 0; deck < 3; ++deck) {
                CAPTURE(transition, position, deck);
                Rig rig;
                rig.mixer.set_crossfader_assign(0, CrossfaderSide::A);
                rig.mixer.set_crossfader_assign(1, CrossfaderSide::B);
                rig.mixer.set_crossfader_curve(transition);
                rig.mixer.set_crossfader(position);
                rig.play(deck);
                check_main(render(rig.mixer, kSettle),
                           value(deck) * crossfader_gain(deck, position, transition));
            }
        }
    }
}

TEST_CASE("Mixer: cue bus and headphone mix", "[mixer]") {
    // Deck 2 is cued, at +6 dB, with its fader down: the headphones hear it
    // alone, and the main bus hears the other decks.
    Rig rig;
    rig.play_all();
    rig.mixer.set_cue(2, true);
    rig.mixer.set_trim(2, 6.0);
    rig.mixer.set_fader(2, 0.0);
    const double cue = value(2) * gain_of_db(6.0);
    const double main = value(0) + value(1) + value(3);

    SECTION("pre-fader cue") {
        const Outputs out = render(rig.mixer, kSettle);
        check_headphones(out, cue);
        check_main(out, main);
    }
    SECTION("cue mix, before the main level") {
        rig.mixer.set_cue_mix(1.0);
        rig.mixer.set_main_level(-6.0);
        const Outputs all_main = render(rig.mixer, kSettle);
        check_headphones(all_main, main);
        check_main(all_main, main * gain_of_db(-6.0));

        rig.mixer.set_cue_mix(0.5);
        check_headphones(render(rig.mixer, kSettle), (0.5 * cue) + (0.5 * main));
    }
    SECTION("headphone level") {
        rig.mixer.set_headphone_level(-6.0);
        const Outputs out = render(rig.mixer, kSettle);
        check_headphones(out, cue * gain_of_db(-6.0));
        check_main(out, main);
    }
    SECTION("main level") {
        rig.mixer.set_main_level(-6.0);
        const Outputs out = render(rig.mixer, kSettle);
        check_headphones(out, cue);
        check_main(out, main * gain_of_db(-6.0));
    }
}

TEST_CASE("Mixer: gain changes are smoothed", "[mixer]") {
    Rig rig;
    rig.play_all();
    static_cast<void>(render(rig.mixer, kSettle));

    rig.mixer.set_fader(0, 0.0);
    const Outputs out = render(rig.mixer, kSettle);

    // Requirement 10, in double: each frame, the gain moves, then the frame
    // is mixed.
    const double step = -std::expm1(-1.0 / (0.001 * ks::engine::test::kRate));
    double gain = 1.0;
    for (std::size_t frame = 0; frame < kSettle; ++frame) {
        gain += (0.0 - gain) * step;
        if (std::abs(gain) <= 1e-6) {
            gain = 0.0;
        }
        const double expected = (gain * value(0)) + value(1) + value(2) + value(3);
        CAPTURE(frame, gain);
        REQUIRE(near(out.at(0).at(frame), expected));
        REQUIRE(near(out.at(1).at(frame), -expected));
    }

    // Settled, it's the same as a fader that was down from the start.
    Rig fresh;
    fresh.mixer.set_fader(0, 0.0);
    fresh.play_all();
    const Outputs settled = render(rig.mixer, 500);
    const Outputs reference = render(fresh.mixer, kSettle + 500);
    for (std::size_t c = 0; c < 4; ++c) {
        for (std::size_t f = 0; f < 500; ++f) {
            CAPTURE(c, f);
            REQUIRE(std::bit_cast<std::uint32_t>(settled.at(c).at(f)) ==
                    std::bit_cast<std::uint32_t>(reference.at(c).at(kSettle + f)));
        }
    }
}

TEST_CASE("Mixer: controls are validated", "[mixer]") {
    Rig rig;
    Mixer& mixer = rig.mixer;
    const MixerSettings defaults = mixer.settings();

    // NaN, and a deck index of kDecks, change nothing.
    mixer.set_trim(0, kNaN);
    mixer.set_fader(0, kNaN);
    mixer.set_crossfader(kNaN);
    mixer.set_crossfader_curve(kNaN);
    mixer.set_main_level(kNaN);
    mixer.set_headphone_level(kNaN);
    mixer.set_cue_mix(kNaN);
    mixer.set_trim(ks::engine::kDecks, 3.0);
    mixer.set_fader(ks::engine::kDecks, 0.5);
    mixer.set_crossfader_assign(ks::engine::kDecks, CrossfaderSide::A);
    mixer.set_cue(ks::engine::kDecks, true);
    REQUIRE(mixer.settings() == defaults);

    // Out-of-range values are clamped.
    mixer.set_trim(0, 20.0);
    mixer.set_trim(1, -kInfinity);
    mixer.set_fader(0, 2.0);
    mixer.set_fader(1, -1.0);
    mixer.set_crossfader(-1.0);
    mixer.set_crossfader_curve(0.0);
    mixer.set_main_level(6.0);
    mixer.set_headphone_level(6.0);
    mixer.set_cue_mix(2.0);
    const MixerSettings& settings = mixer.settings();
    REQUIRE(settings.channels.at(0).trim_db == 12.0);
    REQUIRE(settings.channels.at(1).trim_db == -kInfinity);
    REQUIRE(settings.channels.at(0).fader == 1.0);
    REQUIRE(settings.channels.at(1).fader == 0.0);
    REQUIRE(settings.crossfader == 0.0);
    REQUIRE(settings.crossfader_transition == 0.01);
    REQUIRE(settings.main_level_db == 0.0);
    REQUIRE(settings.headphone_level_db == 0.0);
    REQUIRE(settings.cue_mix == 1.0);

    // Valid values are kept as they are.
    mixer.set_trim(2, -3.5);
    mixer.set_crossfader_assign(2, CrossfaderSide::B);
    mixer.set_cue(2, true);
    REQUIRE(settings.channels.at(2).trim_db == -3.5);
    REQUIRE(settings.channels.at(2).side == CrossfaderSide::B);
    REQUIRE(settings.channels.at(2).cue);
}

TEST_CASE("Mixer: output doesn't depend on block size", "[mixer]") {
    using enum MixerAction;
    constexpr std::uint64_t kFrames = 48'000;
    // Two decks playing music-like tracks: the crossfader sweeps, then cuts;
    // faders, trims, cue, and levels move; a deck seeks and changes rate.
    constexpr std::array<MixerControl, 27> kScript{{
        {.frame = 0, .action = Side, .deck = 0, .value = 0.0},
        {.frame = 0, .action = Side, .deck = 1, .value = 1.0},
        {.frame = 0, .action = Crossfader, .deck = 0, .value = 0.0},
        {.frame = 0, .action = Play, .deck = 0, .value = 0.0},
        {.frame = 0, .action = Play, .deck = 1, .value = 0.0},
        {.frame = 4'800, .action = Crossfader, .deck = 0, .value = 0.25},
        {.frame = 9'600, .action = Crossfader, .deck = 0, .value = 0.5},
        {.frame = 12'000, .action = Fader, .deck = 1, .value = 0.6},
        {.frame = 14'400, .action = Crossfader, .deck = 0, .value = 0.75},
        {.frame = 16'000, .action = Trim, .deck = 0, .value = -3.0},
        {.frame = 19'200, .action = Crossfader, .deck = 0, .value = 1.0},
        {.frame = 20'000, .action = Cue, .deck = 1, .value = 1.0},
        {.frame = 22'000, .action = CueMix, .deck = 0, .value = 0.5},
        {.frame = 24'000, .action = Curve, .deck = 0, .value = 0.05},
        {.frame = 26'000, .action = Crossfader, .deck = 0, .value = 0.0},
        {.frame = 26'500, .action = Crossfader, .deck = 0, .value = 1.0},
        {.frame = 27'000, .action = Crossfader, .deck = 0, .value = 0.0},
        {.frame = 27'250, .action = Crossfader, .deck = 0, .value = 0.97},
        {.frame = 30'000, .action = Seek, .deck = 0, .value = 10'000.0},
        {.frame = 32'000, .action = Rate, .deck = 1, .value = 1.06},
        {.frame = 34'000, .action = Fader, .deck = 0, .value = 0.3},
        {.frame = 36'000, .action = Cue, .deck = 1, .value = 0.0},
        {.frame = 36'000, .action = Cue, .deck = 0, .value = 1.0},
        {.frame = 38'000, .action = MainLevel, .deck = 0, .value = -6.0},
        {.frame = 40'000, .action = HeadphoneLevel, .deck = 0, .value = -3.0},
        {.frame = 42'000, .action = Side, .deck = 1, .value = 2.0},
        {.frame = 44'000, .action = Pause, .deck = 0, .value = 0.0},
    }};

    const auto run = [&kScript](std::size_t block_frames) {
        const Interpolator interpolator;
        const auto music = [](std::uint64_t offset) {
            return [offset](std::uint64_t frame, std::size_t channel) {
                return ks::engine::test::synthetic(frame + offset, channel,
                                                   ks::engine::test::kTrackRate);
            };
        };
        OwnedTrack first{100'000, ks::engine::test::kTrackRate, music(0)};
        OwnedTrack second{100'000, ks::engine::test::kTrackRate, music(7'777)};
        Mixer mixer{interpolator};
        REQUIRE(mixer.deck(0).load(first.track()));
        REQUIRE(mixer.deck(1).load(second.track()));
        return ks::engine::test::perform(mixer, kScript, kFrames, block_frames);
    };

    const Outputs reference = run(1);
    // It played: both buses carry sound.
    REQUIRE(std::ranges::any_of(reference.at(0), [](float sample) { return sample != 0.0F; }));
    REQUIRE(std::ranges::any_of(reference.at(2), [](float sample) { return sample != 0.0F; }));

    for (const std::size_t block_frames : std::array<std::size_t, 3>{37, 128, 512}) {
        CAPTURE(block_frames);
        const Outputs out = run(block_frames);
        for (std::size_t c = 0; c < 4; ++c) {
            for (std::size_t f = 0; f < kFrames; ++f) {
                if (std::bit_cast<std::uint32_t>(out.at(c).at(f)) !=
                    std::bit_cast<std::uint32_t>(reference.at(c).at(f))) {
                    FAIL("channel " << c << ", frame " << f << " differs");
                }
            }
        }
    }
}

TEST_CASE("Mixer: the crossfade matches its golden file", "[mixer][golden]") {
    using enum MixerAction;
    using ks::engine::test::kRate;
    using ks::engine::test::kTrackRate;
    constexpr std::uint64_t kSecond = kRate;
    constexpr std::uint64_t kFrames = 3 * kSecond;

    // Deck 0 on side A at rate 1, and deck 1 on side B at rate 1.06, with the
    // crossfader on A.
    std::vector<MixerControl> script{
        {.frame = 0, .action = Side, .deck = 0, .value = 0.0},
        {.frame = 0, .action = Side, .deck = 1, .value = 1.0},
        {.frame = 0, .action = Crossfader, .deck = 0, .value = 0.0},
        {.frame = 0, .action = Rate, .deck = 1, .value = 1.06},
        {.frame = 0, .action = Play, .deck = 0, .value = 0.0},
        {.frame = 0, .action = Play, .deck = 1, .value = 0.0},
    };
    // From 0.25 s to 1.25 s, a constant-power sweep from A to B, in steps of
    // 5 ms, as a controller sends it.
    constexpr std::uint64_t kSweepStart = kSecond / 4;
    constexpr std::uint64_t kSweepSteps = 200;
    for (std::uint64_t step = 1; step <= kSweepSteps; ++step) {
        script.push_back({.frame = kSweepStart + (step * kSecond / kSweepSteps),
                          .action = Crossfader,
                          .deck = 0,
                          .value = static_cast<double>(step) / kSweepSteps});
    }
    // From 1.5 s, scratch cuts at transition 0.05: A, B, A, ... every 0.125 s.
    constexpr std::uint64_t kCutStart = 3 * kSecond / 2;
    constexpr std::uint64_t kCutEvery = kSecond / 8;
    script.push_back({.frame = kCutStart, .action = Curve, .deck = 0, .value = 0.05});
    for (std::uint64_t cut = 0; kCutStart + (cut * kCutEvery) < kFrames; ++cut) {
        script.push_back({.frame = kCutStart + (cut * kCutEvery),
                          .action = Crossfader,
                          .deck = 0,
                          .value = cut % 2 == 0 ? 0.0 : 1.0});
    }

    // The synthetic deck-test tracks, the second starting 0.125 s in.
    const auto music = [](std::uint64_t offset) {
        return [offset](std::uint64_t frame, std::size_t channel) {
            return ks::engine::test::synthetic(frame + offset, channel, kTrackRate);
        };
    };
    OwnedTrack first{4ULL * kTrackRate, kTrackRate, music(0)};
    OwnedTrack second{4ULL * kTrackRate, kTrackRate, music(kTrackRate / 8)};
    const Interpolator interpolator;
    Mixer mixer{interpolator};
    REQUIRE(mixer.deck(0).load(first.track()));
    REQUIRE(mixer.deck(1).load(second.track()));
    ks::engine::test::ScriptedMixer graph{mixer, script};
    ks::engine::RenderCore core{
        graph, ks::engine::SafetyConfig{.fade = std::chrono::milliseconds{0}, .ceiling_dbfs = 0.0}};

    // The offline driver, one call between each pair of controls, so each
    // applies at its frame.
    std::vector<float> left(kFrames, 5.0F);
    std::vector<float> right(kFrames, 5.0F);
    std::uint64_t frame = 0;
    for (std::size_t next = 0; frame < kFrames;) {
        while (next < script.size() && script.at(next).frame <= frame) {
            ++next;
        }
        const std::uint64_t until = next < script.size() ? script.at(next).frame : kFrames;
        const std::array<std::span<float>, 2> views{std::span{left}.subspan(frame, until - frame),
                                                    std::span{right}.subspan(frame, until - frame)};
        const ks::engine::OfflineConfig config{.sample_rate = kRate,
                                               .block_frames = 64,
                                               .start_time =
                                                   std::chrono::steady_clock::time_point{} +
                                                   ks::engine::frames_to_duration(frame, kRate),
                                               .driver = ks::engine::kDefaultDriver};
        REQUIRE(ks::engine::render_offline(core, config, views) == until - frame);
        frame = until;
    }
    // It played.
    REQUIRE(std::ranges::any_of(left, [](float sample) { return sample != 0.0F; }));

    const std::filesystem::path golden =
        std::filesystem::path{KS_MIXER_GOLDEN_DIR} / "crossfade.wav";
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing in the tests sets the environment
    const char* const update = std::getenv("KS_UPDATE_GOLDEN");
    if (update != nullptr && std::string_view{update} == "1") {
        std::filesystem::create_directories(golden.parent_path());
        REQUIRE(ks::engine::test::write_wav(golden, kRate, left, right));
        SKIP("Wrote " << golden.string() << ": listen to it, review it, and commit it.");
    }

    const auto read = ks::engine::test::read_wav(golden);
    CAPTURE(golden.string());
    REQUIRE(read.has_value());
    const ks::engine::test::WavAudio audio = read.value_or(ks::engine::test::WavAudio{});
    REQUIRE(audio.sample_rate == kRate);
    REQUIRE(audio.samples.size() == 2 * kFrames);
    for (std::size_t f = 0; f < kFrames; ++f) {
        const double left_error = std::abs(static_cast<double>(left.at(f)) -
                                           static_cast<double>(audio.samples.at(2 * f)));
        const double right_error = std::abs(static_cast<double>(right.at(f)) -
                                            static_cast<double>(audio.samples.at((2 * f) + 1)));
        if (left_error > 1e-5 || right_error > 1e-5) {
            FAIL("frame " << f << " differs from the golden file by " << left_error << ", "
                          << right_error);
        }
    }
}
