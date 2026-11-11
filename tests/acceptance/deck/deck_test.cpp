// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/deck/spec.md: the deck.

#include <ks/engine/deck.h>

#include <ks/engine/block.h>
#include <ks/engine/interpolator.h>
#include <ks/engine/offline.h>
#include <ks/engine/render_core.h>
#include <ks/engine/safety.h>
#include <ks/engine/track.h>

#include "test_support.h"
#include "wav.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/mman.h>

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

using ks::engine::Deck;
using ks::engine::Interpolator;
using ks::engine::kMaxTrackFrames;
using ks::engine::Track;
using ks::engine::test::kPerformance;
using ks::engine::test::kPerformanceFrames;
using ks::engine::test::kPerformanceTrackFrames;
using ks::engine::test::kRampFrames;
using ks::engine::test::kRate;
using ks::engine::test::kSignal;
using ks::engine::test::kTrackRate;
using ks::engine::test::near;
using ks::engine::test::OwnedTrack;
using ks::engine::test::Performance;
using ks::engine::test::render;
using ks::engine::test::render_tracking;
using ks::engine::test::Stereo;

/// The gain k frames into a ramp up.
double ramp_up(std::size_t k) {
    return k >= kRampFrames ? 1.0 : static_cast<double>(k) / static_cast<double>(kRampFrames);
}

/// The gain k frames into a ramp down.
double ramp_down(std::size_t k) {
    return 1.0 - ramp_up(k);
}

/// Loads `track`, seeks to `frame`, and plays for `frames` frames, so the
/// deck is audible at full gain.
void play_from(Deck& deck, Track& track, double frame, std::size_t frames = 1'000) {
    REQUIRE(deck.load(track));
    deck.seek(frame);
    deck.set_playing(true);
    static_cast<void>(render(deck, frames));
}

/// The performance's track.
OwnedTrack& performance_track() {
    static OwnedTrack owned{kPerformanceTrackFrames, kTrackRate,
                            [](std::uint64_t frame, std::size_t channel) {
                                return ks::engine::test::synthetic(frame, channel, kTrackRate);
                            }};
    return owned;
}

} // namespace

TEST_CASE("Deck: play and pause ramp the gain", "[deck]") {
    const Interpolator interpolator;
    OwnedTrack owned{20'000, kRate, kSignal};
    Deck deck{interpolator};
    REQUIRE(deck.load(owned.track()));
    deck.seek(1'000.0);
    deck.set_playing(true);

    const Stereo playing = render(deck, 1'000);
    for (std::size_t channel = 0; channel < 2; ++channel) {
        for (std::size_t k = 0; k < 1'000; ++k) {
            CAPTURE(channel, k);
            const float sample = owned.sample(1'000 + k, channel);
            if (k < kRampFrames) {
                REQUIRE(near(playing.at(channel, k), static_cast<double>(sample) * ramp_up(k)));
            } else {
                REQUIRE(playing.at(channel, k) == sample);
            }
        }
    }

    deck.set_playing(false);
    REQUIRE_FALSE(deck.playing());
    const Stereo pausing = render(deck, 1'000);
    for (std::size_t channel = 0; channel < 2; ++channel) {
        for (std::size_t k = 0; k < 1'000; ++k) {
            CAPTURE(channel, k);
            const auto sample = static_cast<double>(owned.sample(2'000 + k, channel));
            if (k < kRampFrames) {
                REQUIRE(near(pausing.at(channel, k), sample * ramp_down(k)));
            } else {
                REQUIRE(pausing.at(channel, k) == 0.0F);
            }
        }
    }
    REQUIRE(deck.position() == 2'000.0 + kRampFrames);
}

TEST_CASE("Deck: seeking crossfades", "[deck]") {
    const Interpolator interpolator;
    OwnedTrack owned{40'000, kRate, kSignal};
    Deck deck{interpolator};
    play_from(deck, owned.track(), 1'000.0); // now at 2,000, at full gain

    // The crossfade's frame k: old at 2,000 + k, new at 10,000 + k.
    const auto crossfade = [&owned](std::size_t k, std::size_t channel, std::uint64_t from,
                                    std::uint64_t to) {
        return (ramp_down(k) * static_cast<double>(owned.sample(from + k, channel))) +
               (ramp_up(k) * static_cast<double>(owned.sample(to + k, channel)));
    };

    SECTION("while playing, and during a crossfade") {
        deck.seek(10'000.0);
        const Stereo first = render_tracking(deck, 100);
        for (std::size_t k = 0; k < 100; ++k) {
            CAPTURE(k);
            REQUIRE(first.positions.at(k) == 10'000.0 + static_cast<double>(k));
            for (std::size_t channel = 0; channel < 2; ++channel) {
                REQUIRE(near(first.at(channel, k), crossfade(k, channel, 2'000, 10'000)));
            }
        }

        // A second seek waits for the first crossfade to finish.
        deck.seek(30'000.0);
        const Stereo rest = render_tracking(deck, 600);
        for (std::size_t k = 100; k < kRampFrames; ++k) {
            CAPTURE(k);
            REQUIRE(rest.positions.at(k - 100) == 10'000.0 + static_cast<double>(k));
            for (std::size_t channel = 0; channel < 2; ++channel) {
                REQUIRE(near(rest.at(channel, k - 100), crossfade(k, channel, 2'000, 10'000)));
            }
        }
        // Then crossfades from 10,240 to 30,000.
        for (std::size_t j = 0; j < 460; ++j) {
            CAPTURE(j);
            const std::size_t at = (kRampFrames - 100) + j;
            REQUIRE(rest.positions.at(at) == 30'000.0 + static_cast<double>(j));
            for (std::size_t channel = 0; channel < 2; ++channel) {
                if (j < kRampFrames) {
                    REQUIRE(near(rest.at(channel, at), crossfade(j, channel, 10'240, 30'000)));
                } else {
                    REQUIRE(rest.at(channel, at) == owned.sample(30'000 + j, channel));
                }
            }
        }
    }

    SECTION("while paused") {
        deck.set_playing(false);
        static_cast<void>(render(deck, 300));
        deck.seek(5'000.0);
        REQUIRE(deck.position() == 5'000.0);
        REQUIRE(render(deck, 100).silent());
        REQUIRE(deck.position() == 5'000.0);
    }
}

TEST_CASE("Deck: rate changes are smoothed", "[deck]") {
    const Interpolator interpolator;
    OwnedTrack owned{100'000, kRate, kSignal};

    SECTION("while playing") {
        Deck deck{interpolator};
        play_from(deck, owned.track(), 1'000.0); // now at 2,000
        deck.set_rate(2.0);
        static_cast<void>(render(deck, kRate / 10));

        // Requirement 10, in double: each frame, the rate moves, then the
        // position advances by it.
        const double step = -std::expm1(-1.0 / (0.005 * kRate));
        double rate = 1.0;
        double position = 2'000.0;
        for (std::uint32_t frame = 0; frame < kRate / 10; ++frame) {
            rate += (2.0 - rate) * step;
            if (std::abs(2.0 - rate) <= 1e-9) {
                rate = 2.0;
            }
            position += rate;
        }
        CAPTURE(deck.position(), position);
        REQUIRE(std::abs(deck.position() - position) <= 1e-3);

        // By 0.25 s, exactly 2 per frame.
        static_cast<void>(render(deck, (kRate * 15) / 100));
        const Stereo settled = render_tracking(deck, 100);
        for (std::size_t k = 1; k < 100; ++k) {
            CAPTURE(k);
            REQUIRE(settled.positions.at(k) - settled.positions.at(k - 1) == 2.0);
        }
    }

    SECTION("while silent, at once") {
        Deck deck{interpolator};
        REQUIRE(deck.load(owned.track()));
        deck.set_rate(2.0);
        deck.seek(1'000.0);
        deck.set_playing(true);
        const Stereo out = render_tracking(deck, 100);
        for (std::size_t k = 0; k < 100; ++k) {
            CAPTURE(k);
            REQUIRE(out.positions.at(k) == 1'000.0 + (2.0 * static_cast<double>(k)));
        }
    }

    SECTION("validation, and loading keeps the rate") {
        Deck deck{interpolator};
        deck.set_rate(10.0);
        REQUIRE(deck.rate() == 4.0);
        deck.set_rate(std::numeric_limits<double>::quiet_NaN());
        REQUIRE(deck.rate() == 4.0);
        deck.set_rate(-std::numeric_limits<double>::infinity());
        REQUIRE(deck.rate() == 4.0);
        deck.set_rate(-10.0);
        REQUIRE(deck.rate() == -4.0);
        REQUIRE(deck.load(owned.track()));
        REQUIRE(deck.rate() == -4.0);
    }
}

TEST_CASE("Deck: stops at the ends", "[deck]") {
    const Interpolator interpolator;
    OwnedTrack owned{1'000, kRate, kSignal};

    SECTION("forward") {
        Deck deck{interpolator};
        REQUIRE(deck.load(owned.track()));
        deck.seek(900.0);
        deck.set_playing(true);
        const Stereo out = render_tracking(deck, 400);
        for (std::size_t k = 100; k < 400; ++k) {
            CAPTURE(k);
            REQUIRE(out.positions.at(k) == 1'000.0);
            REQUIRE(out.left.at(k) == 0.0F);
            REQUIRE(out.right.at(k) == 0.0F);
        }
        REQUIRE_FALSE(deck.playing());

        // Playing at the end stops at once.
        deck.set_playing(true);
        static_cast<void>(render(deck, 10));
        REQUIRE_FALSE(deck.playing());
        REQUIRE(deck.position() == 1'000.0);

        // In reverse, it plays.
        deck.set_rate(-1.0);
        deck.set_playing(true);
        static_cast<void>(render(deck, 100));
        REQUIRE(deck.playing());
        REQUIRE(deck.position() == 900.0);
    }

    SECTION("in reverse") {
        Deck deck{interpolator};
        REQUIRE(deck.load(owned.track()));
        deck.set_rate(-1.0);
        deck.seek(500.0);
        deck.set_playing(true);
        const Stereo out = render_tracking(deck, 1'000);
        for (std::size_t k = 0; k < 500; ++k) {
            CAPTURE(k);
            REQUIRE(out.positions.at(500 + k) == 0.0);
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto first = static_cast<double>(owned.sample(0, channel));
                REQUIRE(near(out.at(channel, 500 + k), first * ramp_down(k)));
            }
        }
        REQUIRE_FALSE(deck.playing());
    }

    SECTION("a seek past the end") {
        Deck deck{interpolator};
        REQUIRE(deck.load(owned.track()));
        deck.seek(5'000.0);
        REQUIRE(deck.position() == 1'000.0);
    }
}

TEST_CASE("Deck: waits for decoding", "[deck]") {
    const Interpolator interpolator;
    OwnedTrack owned{48'000, kRate, kSignal};
    owned.track().publish(10'000, false);
    Deck deck{interpolator};
    REQUIRE(deck.load(owned.track()));
    deck.seek(9'000.0);
    deck.set_playing(true);

    // Holds at 10,000 - 128, fading out on the held sample.
    const Stereo held = render_tracking(deck, 2'000);
    for (std::size_t k = 0; k < 2'000 - 872; ++k) {
        CAPTURE(k);
        REQUIRE(held.positions.at(872 + k) == 9'872.0);
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto sample = static_cast<double>(owned.sample(9'872, channel));
            REQUIRE(near(held.at(channel, 872 + k), sample * ramp_down(k)));
        }
    }
    REQUIRE(deck.waiting());
    REQUIRE(deck.playing());

    // More frames: it moves on, ramping up.
    owned.track().publish(20'000, false);
    const Stereo resumed = render_tracking(deck, 1'000);
    for (std::size_t k = 0; k < 1'000; ++k) {
        CAPTURE(k);
        REQUIRE(resumed.positions.at(k) == 9'872.0 + static_cast<double>(k));
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto sample = static_cast<double>(owned.sample(9'872 + k, channel));
            REQUIRE(near(resumed.at(channel, k), sample * ramp_up(k)));
        }
    }
    REQUIRE_FALSE(deck.waiting());

    // A seek past the ready frames waits there.
    deck.seek(30'000.0);
    static_cast<void>(render(deck, 2'000));
    REQUIRE(deck.waiting());
    REQUIRE(deck.position() == 30'000.0);

    owned.track().publish(48'000, true);
    const Stereo plays = render_tracking(deck, 1'000);
    for (std::size_t k = 0; k < 1'000; ++k) {
        CAPTURE(k);
        REQUIRE(plays.positions.at(k) == 30'000.0 + static_cast<double>(k));
    }
    REQUIRE_FALSE(deck.waiting());
}

TEST_CASE("Deck: loading and unloading", "[deck]") {
    const Interpolator interpolator;
    OwnedTrack first{20'000, kRate, kSignal};
    OwnedTrack second{20'000, kRate, [](std::uint64_t frame, std::size_t channel) {
                          return kSignal(frame + 100'000, channel);
                      }};

    SECTION("while playing") {
        Deck deck{interpolator};
        play_from(deck, first.track(), 1'000.0); // now at 2,000
        deck.set_cue(500.0);
        REQUIRE(deck.cue() == 500.0);

        REQUIRE(deck.load(second.track()));
        REQUIRE(deck.track() == &second.track());
        REQUIRE_FALSE(deck.playing());
        REQUIRE(deck.position() == 0.0);
        REQUIRE(deck.cue() == 0.0);

        const Stereo fading = render(deck, kRampFrames - 1);
        REQUIRE(deck.holds(first.track()));
        const Stereo last = render(deck, 1);
        REQUIRE_FALSE(deck.holds(first.track()));
        REQUIRE(deck.holds(second.track()));
        for (std::size_t channel = 0; channel < 2; ++channel) {
            for (std::size_t k = 0; k < kRampFrames - 1; ++k) {
                CAPTURE(channel, k);
                const auto sample = static_cast<double>(first.sample(2'000 + k, channel));
                REQUIRE(near(fading.at(channel, k), sample * ramp_down(k)));
            }
            REQUIRE(near(last.at(channel, 0),
                         static_cast<double>(first.sample(2'239, channel)) * ramp_down(239)));
        }
        REQUIRE(render(deck, 100).silent());
        REQUIRE(deck.position() == 0.0);
    }

    SECTION("load and play crossfade between tracks") {
        Deck deck{interpolator};
        play_from(deck, first.track(), 1'000.0); // now at 2,000
        REQUIRE(deck.load(second.track()));
        deck.set_playing(true);
        const Stereo out = render(deck, 500);
        for (std::size_t channel = 0; channel < 2; ++channel) {
            for (std::size_t k = 0; k < 500; ++k) {
                CAPTURE(channel, k);
                const auto old = static_cast<double>(first.sample(2'000 + k, channel));
                const float fresh = second.sample(k, channel);
                if (k < kRampFrames) {
                    REQUIRE(near(out.at(channel, k),
                                 (old * ramp_down(k)) + (static_cast<double>(fresh) * ramp_up(k))));
                } else {
                    REQUIRE(out.at(channel, k) == fresh);
                }
            }
        }
    }

    SECTION("refused tracks") {
        Deck deck{interpolator};
        play_from(deck, first.track(), 1'000.0);
        std::vector<float> samples(2'000, 0.0F);
        const Track too_slow{samples, ks::engine::kMinTrackRate - 1};
        const Track too_fast{samples, ks::engine::kMaxTrackRate + 1};
        REQUIRE_FALSE(deck.load(too_slow));
        REQUIRE_FALSE(deck.load(too_fast));

        // 2^28 + 1 frames of address space, reserved but never touched.
        const std::size_t floats = (kMaxTrackFrames + 1) * 2;
        const std::size_t bytes = floats * sizeof(float);
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
        flags |= MAP_NORESERVE;
#endif
        void* const memory = mmap(nullptr, bytes, PROT_READ, flags, -1, 0);
        REQUIRE(memory != MAP_FAILED);
        {
            const Track too_long{std::span{static_cast<const float*>(memory), floats}, kRate};
            REQUIRE_FALSE(deck.load(too_long));
        }
        REQUIRE(munmap(memory, bytes) == 0);

        // Nothing changed.
        REQUIRE(deck.track() == &first.track());
        REQUIRE(deck.playing());
        REQUIRE_FALSE(render(deck, 100).silent());
    }

    SECTION("unloading while playing") {
        Deck deck{interpolator};
        play_from(deck, first.track(), 1'000.0); // now at 2,000
        deck.unload();
        REQUIRE(deck.track() == nullptr);
        REQUIRE_FALSE(deck.playing());
        const Stereo out = render(deck, 500);
        for (std::size_t channel = 0; channel < 2; ++channel) {
            for (std::size_t k = 0; k < 500; ++k) {
                CAPTURE(channel, k);
                const auto sample = static_cast<double>(first.sample(2'000 + k, channel));
                REQUIRE(near(out.at(channel, k), sample * ramp_down(k)));
            }
        }
        REQUIRE_FALSE(deck.holds(first.track()));
    }

    SECTION("with no track loaded") {
        Deck deck{interpolator};
        deck.set_playing(true);
        deck.seek(100.0);
        deck.set_cue(50.0);
        REQUIRE_FALSE(deck.playing());
        REQUIRE(deck.position() == 0.0);
        REQUIRE(deck.cue() == 0.0);
        REQUIRE(render(deck, 100).silent());
    }
}

TEST_CASE("Deck: output doesn't depend on block size", "[deck]") {
    const Interpolator interpolator;
    const Track& track = performance_track().track();

    // Blocks of 1 frame are the reference.
    Deck reference_deck{interpolator};
    const Performance reference =
        ks::engine::test::perform(reference_deck, track, kPerformance, kPerformanceFrames, 1);
    // It played, and ended at the end of the track.
    REQUIRE_FALSE(reference.output.silent());
    REQUIRE(reference.states.back().position == static_cast<double>(kPerformanceTrackFrames));
    REQUIRE_FALSE(reference.states.back().playing);

    for (const std::size_t block_frames : std::array<std::size_t, 3>{37, 128, 512}) {
        CAPTURE(block_frames);
        Deck deck{interpolator};
        const Performance performance =
            ks::engine::test::perform(deck, track, kPerformance, kPerformanceFrames, block_frames);
        REQUIRE(performance.states == reference.states);
        for (std::size_t frame = 0; frame < kPerformanceFrames; ++frame) {
            if (std::bit_cast<std::uint32_t>(performance.output.left.at(frame)) !=
                    std::bit_cast<std::uint32_t>(reference.output.left.at(frame)) ||
                std::bit_cast<std::uint32_t>(performance.output.right.at(frame)) !=
                    std::bit_cast<std::uint32_t>(reference.output.right.at(frame))) {
                FAIL("frame " << frame << " differs");
            }
        }
    }
}

TEST_CASE("Deck: the performance matches its golden file", "[deck][golden]") {
    using ks::engine::test::Control;
    const Interpolator interpolator;
    Deck deck{interpolator};
    ks::engine::test::ScriptGraph graph{deck, performance_track().track(), kPerformance};
    ks::engine::RenderCore core{
        graph, ks::engine::SafetyConfig{.fade = std::chrono::milliseconds{0}, .ceiling_dbfs = 0.0}};

    // The offline driver, one call between each pair of controls.
    std::vector<float> left(kPerformanceFrames, 5.0F);
    std::vector<float> right(kPerformanceFrames, 5.0F);
    std::uint64_t frame = 0;
    for (std::size_t next = 0; frame < kPerformanceFrames;) {
        while (next < kPerformance.size() && kPerformance.at(next).frame <= frame) {
            ++next;
        }
        const std::uint64_t until =
            next < kPerformance.size() ? kPerformance.at(next).frame : kPerformanceFrames;
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

    const std::filesystem::path golden =
        std::filesystem::path{KS_DECK_GOLDEN_DIR} / "performance.wav";
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
    REQUIRE(audio.samples.size() == 2 * kPerformanceFrames);
    for (std::size_t f = 0; f < kPerformanceFrames; ++f) {
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
