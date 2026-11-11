// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/mixer.h>

#include <ks/engine/block.h>
#include <ks/engine/interpolator.h>
#include <ks/engine/track.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace {

using ks::engine::Block;
using ks::engine::BusMap;
using ks::engine::CrossfaderSide;
using ks::engine::Interpolator;
using ks::engine::kDecks;
using ks::engine::kMaxBlockFrames;
using ks::engine::kMaxTrimDb;
using ks::engine::kMinCrossfaderTransition;
using ks::engine::Mixer;
using ks::engine::MixerSettings;
using ks::engine::Track;

constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::uint32_t kRate = 48'000;
constexpr std::size_t kTrackFrames = 10'000;
/// Enough frames for the decks' play ramps to finish.
constexpr std::size_t kSettle = 2'000;
/// What the output is filled with before rendering, to show what's written.
constexpr float kSentinel = 9.0F;

/// Deck i's constant on the left; the right is its negative.
float deck_value(std::size_t deck) {
    return 0.05F * static_cast<float>(deck + 1);
}

/// A mixer whose decks are loaded with constant tracks.
struct Rig {
    explicit Rig(const BusMap& map = {}) : mixer{interpolator, map} {
        samples.reserve(kDecks);
        for (std::size_t i = 0; i < kDecks; ++i) {
            auto& data = samples.emplace_back(kTrackFrames * 2);
            for (std::size_t frame = 0; frame < kTrackFrames; ++frame) {
                data.at(frame * 2) = deck_value(i);
                data.at((frame * 2) + 1) = -deck_value(i);
            }
            const auto& track = tracks.emplace_back(std::make_unique<Track>(data, kRate));
            track->publish(kTrackFrames, true);
            REQUIRE(mixer.deck(i).load(*track));
        }
    }

    void play_all() {
        for (std::size_t i = 0; i < kDecks; ++i) {
            mixer.deck(i).set_playing(true);
        }
    }

    Interpolator interpolator;
    std::vector<std::vector<float>> samples;
    std::vector<std::unique_ptr<Track>> tracks;
    Mixer mixer;
};

using Outputs = std::vector<std::vector<float>>;

/// Renders `frames` frames of `channels` channels, prefilled with
/// kSentinel, in blocks of `block_frames` at `rate`.
Outputs render(Mixer& mixer, std::size_t frames, std::size_t channels,
               std::size_t block_frames = 64, std::uint32_t rate = kRate) {
    Outputs out(channels, std::vector<float>(frames, kSentinel));
    std::vector<std::span<float>> views(channels);
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min(block_frames, frames - done);
        for (std::size_t c = 0; c < channels; ++c) {
            views.at(c) = std::span{out.at(c)}.subspan(done, n);
        }
        mixer.process(Block{.channels = views,
                            .frames = n,
                            .sample_rate = rate,
                            .host_time = {},
                            .resumed = false});
        done += n;
    }
    return out;
}

/// The smoothing coefficient at `rate`, as requirement 10 defines it.
double smoothing_at(std::uint32_t rate) {
    return -std::expm1(-1.0 / (0.001 * static_cast<double>(rate)));
}

/// One reference smoothing step, in double.
double step_towards(double gain, double target, double smoothing) {
    gain += (target - gain) * smoothing;
    if (std::abs(gain - target) <= 1e-6) {
        gain = target;
    }
    return gain;
}

/// The main bus's left sample with deck 0 at `gain` and the others at 1,
/// summed in the mixer's order.
double main_left_with(double gain) {
    double sum = 0.0;
    sum += gain * static_cast<double>(deck_value(0));
    for (std::size_t i = 1; i < kDecks; ++i) {
        sum += 1.0 * static_cast<double>(deck_value(i));
    }
    return sum;
}

/// A control applied before frame `frame` of a script.
struct Event {
    std::size_t frame;
    std::function<void(Mixer&)> apply;
};

/// Plays all decks, then renders `frames` frames of 8 channels in blocks of
/// at most `block_frames`, split at each event's frame.
Outputs perform(std::size_t block_frames, std::size_t frames, const std::vector<Event>& events) {
    Rig rig;
    rig.play_all();
    Outputs out(8, std::vector<float>(frames, kSentinel));
    std::vector<std::span<float>> views(8);
    std::size_t next = 0;
    for (std::size_t done = 0; done < frames;) {
        while (next < events.size() && events.at(next).frame <= done) {
            events.at(next).apply(rig.mixer);
            ++next;
        }
        std::size_t n = std::min(block_frames, frames - done);
        if (next < events.size()) {
            n = std::min(n, events.at(next).frame - done);
        }
        for (std::size_t c = 0; c < views.size(); ++c) {
            views.at(c) = std::span{out.at(c)}.subspan(done, n);
        }
        rig.mixer.process(Block{.channels = views,
                                .frames = n,
                                .sample_rate = kRate,
                                .host_time = {},
                                .resumed = false});
        done += n;
    }
    return out;
}

bool all_equal(const std::vector<float>& channel, float value) {
    return std::ranges::all_of(channel, [value](float sample) { return sample == value; });
}

bool near(float actual, double expected) {
    return std::abs(static_cast<double>(actual) - expected) <= 1e-6;
}

TEST_CASE("Mixer: an 8-channel block with map {4, 6} writes only channels 4 to 7", "[mixer]") {
    Rig rig{BusMap{.main = 4, .cue = 6}};
    rig.play_all();
    const Outputs out = render(rig.mixer, kSettle, 8);
    for (std::size_t c = 0; c < 4; ++c) {
        CAPTURE(c);
        REQUIRE(all_equal(out.at(c), kSentinel));
    }
    REQUIRE(near(out.at(4).back(), 0.5));
    REQUIRE(near(out.at(5).back(), -0.5));
    // Nothing is cued, and the cue mix is 0: the headphones are silent.
    REQUIRE(all_equal(out.at(6), 0.0F));
    REQUIRE(all_equal(out.at(7), 0.0F));
}

TEST_CASE("Mixer: a 3-channel block writes only the main bus", "[mixer]") {
    Rig rig;
    rig.play_all();
    const Outputs out = render(rig.mixer, kSettle, 3);
    REQUIRE(near(out.at(0).back(), 0.5));
    REQUIRE(near(out.at(1).back(), -0.5));
    REQUIRE(all_equal(out.at(2), kSentinel));
}

TEST_CASE("Mixer: an invalid block writes nothing and renders nothing", "[mixer]") {
    Rig rig;
    rig.play_all();
    std::vector<std::vector<float>> out(4, std::vector<float>(kMaxBlockFrames + 1, kSentinel));
    std::vector<std::span<float>> views;
    views.reserve(out.size());
    for (auto& channel : out) {
        views.emplace_back(channel);
    }
    const auto block = [&views](std::size_t frames, std::uint32_t sample_rate) {
        return Block{.channels = views,
                     .frames = frames,
                     .sample_rate = sample_rate,
                     .host_time = {},
                     .resumed = false};
    };

    rig.mixer.process(block(0, kRate));
    rig.mixer.process(block(kMaxBlockFrames + 1, kRate));
    rig.mixer.process(block(64, 0));
    // The cue bus's right channel is shorter than the block.
    views.at(3) = views.at(3).first(10);
    rig.mixer.process(block(64, kRate));

    for (const auto& channel : out) {
        REQUIRE(all_equal(channel, kSentinel));
    }
    for (std::size_t i = 0; i < kDecks; ++i) {
        REQUIRE(rig.mixer.deck(i).position() == 0.0);
    }
}

TEST_CASE("Mixer: paused decks give silence", "[mixer]") {
    Rig rig;
    const Outputs out = render(rig.mixer, 256, 4);
    for (const auto& channel : out) {
        REQUIRE(all_equal(channel, 0.0F));
    }
}

TEST_CASE("Mixer: a main level of -inf silences main but not the headphones", "[mixer]") {
    Rig rig;
    rig.play_all();
    rig.mixer.set_main_level(-kInfinity);
    rig.mixer.set_cue(0, true);

    // The main level smooths down from its default of 0 dB, then is exactly
    // 0 once it has settled.
    const auto settled = [](const std::vector<float>& channel) {
        return std::vector<float>(channel.begin() + (kSettle / 2), channel.end());
    };

    SECTION("cue mix 0: the headphones carry the cued deck") {
        const Outputs out = render(rig.mixer, kSettle, 4);
        REQUIRE(all_equal(settled(out.at(0)), 0.0F));
        REQUIRE(all_equal(settled(out.at(1)), 0.0F));
        REQUIRE(near(out.at(2).back(), static_cast<double>(deck_value(0))));
        REQUIRE(near(out.at(3).back(), -static_cast<double>(deck_value(0))));
    }
    SECTION("cue mix 1: the headphones carry the main bus before the main level") {
        rig.mixer.set_cue_mix(1.0);
        const Outputs out = render(rig.mixer, kSettle, 4);
        REQUIRE(all_equal(settled(out.at(0)), 0.0F));
        REQUIRE(near(out.at(2).back(), 0.5));
        REQUIRE(near(out.at(3).back(), -0.5));
    }
}

TEST_CASE("Mixer: a fader move is smoothed per sample, whatever the block size", "[mixer]") {
    for (const std::size_t block_frames : {std::size_t{1}, std::size_t{37}, std::size_t{64}}) {
        CAPTURE(block_frames);
        Rig rig;
        rig.play_all();
        static_cast<void>(render(rig.mixer, kSettle, 2, block_frames));

        rig.mixer.set_fader(0, 0.5);
        const Outputs out = render(rig.mixer, kSettle, 2, block_frames);

        const double smoothing = smoothing_at(kRate);
        double gain = 1.0;
        for (std::size_t frame = 0; frame < kSettle; ++frame) {
            gain = step_towards(gain, 0.25, smoothing);
            CAPTURE(frame, gain);
            REQUIRE(near(out.at(0).at(frame), main_left_with(gain)));
        }
        // Settled exactly on the target.
        REQUIRE(gain == 0.25);
        REQUIRE(out.at(0).back() == static_cast<float>(main_left_with(0.25)));
    }
}

TEST_CASE("Mixer: a gain within 1e-6 of its target snaps to it", "[mixer]") {
    // Deck 0 alone, so a gain difference near 1e-6 shows in a float sample.
    Rig rig;
    for (std::size_t i = 1; i < kDecks; ++i) {
        rig.mixer.set_fader(i, 0.0);
    }
    rig.play_all();
    static_cast<void>(render(rig.mixer, kSettle, 2));

    // A trim whose gain is about 9e-7 below 1: the first step lands within
    // 1e-6, so the very first frame uses the exact target.
    const double db = 20.0 * std::log10(1.0 - 9e-7);
    const double target = std::pow(10.0, db / 20.0);
    const auto value = static_cast<double>(deck_value(0));
    const auto snapped = static_cast<float>(target * value);
    const double unsnapped = 1.0 + ((target - 1.0) * smoothing_at(kRate));
    REQUIRE(static_cast<float>(unsnapped * value) != snapped);

    rig.mixer.set_trim(0, db);
    const Outputs out = render(rig.mixer, 4, 2);
    for (const float sample : out.at(0)) {
        REQUIRE(sample == snapped);
    }
}

TEST_CASE("Mixer: a sample-rate change recomputes the smoothing and keeps the gains", "[mixer]") {
    constexpr std::uint32_t kHigherRate = 96'000;
    constexpr std::size_t kBefore = 10;
    // The decks restart their play ramps at a new rate, all alike, so a
    // reference rig without the fader move gives the decks' common level.
    Rig rig;
    Rig reference;
    rig.play_all();
    reference.play_all();
    static_cast<void>(render(rig.mixer, kSettle, 2));
    static_cast<void>(render(reference.mixer, kSettle, 2));

    rig.mixer.set_fader(0, 0.0);
    const Outputs before = render(rig.mixer, kBefore, 2);
    const Outputs after = render(rig.mixer, 500, 2, 64, kHigherRate);
    static_cast<void>(render(reference.mixer, kBefore, 2));
    const Outputs level = render(reference.mixer, 500, 2, 64, kHigherRate);
    const auto deck_level = [&level](std::size_t frame) {
        return static_cast<double>(level.at(0).at(frame)) / main_left_with(1.0);
    };

    double gain = 1.0;
    for (std::size_t frame = 0; frame < kBefore; ++frame) {
        gain = step_towards(gain, 0.0, smoothing_at(kRate));
        CAPTURE(frame, gain);
        REQUIRE(near(before.at(0).at(frame), main_left_with(gain)));
    }
    // Had the coefficient not been recomputed, the first frame at the new
    // rate would be here instead.
    const double stale =
        main_left_with(step_towards(gain, 0.0, smoothing_at(kRate))) * deck_level(0);
    REQUIRE_FALSE(near(after.at(0).front(), stale));
    for (std::size_t frame = 0; frame < after.at(0).size(); ++frame) {
        gain = step_towards(gain, 0.0, smoothing_at(kHigherRate));
        CAPTURE(frame, gain, deck_level(frame));
        REQUIRE(near(after.at(0).at(frame), main_left_with(gain) * deck_level(frame)));
    }
}

TEST_CASE("Mixer: a cue toggle and a bus map change keep block-size invariance", "[mixer]") {
    const std::vector<Event> events{
        {.frame = 100, .apply = [](Mixer& mixer) { mixer.set_cue(1, true); }},
        {.frame = 100, .apply = [](Mixer& mixer) { mixer.set_cue_mix(0.3); }},
        {.frame = 250,
         .apply = [](Mixer& mixer) { mixer.set_bus_map(BusMap{.main = 4, .cue = 6}); }},
        {.frame = 260, .apply = [](Mixer& mixer) { mixer.set_fader(2, 0.2); }},
        {.frame = 400, .apply = [](Mixer& mixer) { mixer.set_cue(1, false); }},
    };
    constexpr std::size_t kFrames = 1'000;
    const Outputs reference = perform(1, kFrames, events);
    for (const std::size_t block_frames : {std::size_t{37}, std::size_t{64}}) {
        const Outputs out = perform(block_frames, kFrames, events);
        for (std::size_t c = 0; c < out.size(); ++c) {
            for (std::size_t f = 0; f < kFrames; ++f) {
                CAPTURE(block_frames, c, f);
                REQUIRE(std::bit_cast<std::uint32_t>(out.at(c).at(f)) ==
                        std::bit_cast<std::uint32_t>(reference.at(c).at(f)));
            }
        }
    }
    // The map change moved both buses at frame 250.
    REQUIRE(reference.at(0).at(249) != kSentinel);
    REQUIRE(reference.at(0).at(250) == kSentinel);
    REQUIRE(reference.at(4).at(249) == kSentinel);
    REQUIRE(reference.at(4).at(250) != kSentinel);
}

TEST_CASE("Mixer: the defaults are MixerSettings{}", "[mixer]") {
    const Interpolator interpolator;
    const Mixer mixer{interpolator};
    REQUIRE(mixer.settings() == MixerSettings{});
}

TEST_CASE("Mixer: per-channel controls clamp at and past their bounds", "[mixer]") {
    const Interpolator interpolator;
    Mixer mixer{interpolator};
    const auto& channel = mixer.settings().channels.at(1);

    mixer.set_trim(1, kMaxTrimDb);
    REQUIRE(channel.trim_db == kMaxTrimDb);
    mixer.set_trim(1, 12.5);
    REQUIRE(channel.trim_db == kMaxTrimDb);
    mixer.set_trim(1, kInfinity);
    REQUIRE(channel.trim_db == kMaxTrimDb);
    mixer.set_trim(1, -kInfinity);
    REQUIRE(channel.trim_db == -kInfinity);
    mixer.set_trim(1, -60.0);
    REQUIRE(channel.trim_db == -60.0);

    mixer.set_fader(1, 0.0);
    REQUIRE(channel.fader == 0.0);
    mixer.set_fader(1, 1.0);
    REQUIRE(channel.fader == 1.0);
    mixer.set_fader(1, 1.5);
    REQUIRE(channel.fader == 1.0);
    mixer.set_fader(1, kInfinity);
    REQUIRE(channel.fader == 1.0);
    mixer.set_fader(1, -0.5);
    REQUIRE(channel.fader == 0.0);
    mixer.set_fader(1, -kInfinity);
    REQUIRE(channel.fader == 0.0);
    mixer.set_fader(1, 0.3);
    REQUIRE(channel.fader == 0.3);

    mixer.set_crossfader_assign(1, CrossfaderSide::A);
    REQUIRE(channel.side == CrossfaderSide::A);
    mixer.set_crossfader_assign(1, CrossfaderSide::B);
    REQUIRE(channel.side == CrossfaderSide::B);
    mixer.set_crossfader_assign(1, CrossfaderSide::Thru);
    REQUIRE(channel.side == CrossfaderSide::Thru);

    mixer.set_cue(1, true);
    REQUIRE(channel.cue);
    mixer.set_cue(1, false);
    REQUIRE_FALSE(channel.cue);

    // Only deck 1 changed.
    REQUIRE(mixer.settings().channels.at(0) == ks::engine::ChannelSettings{});
    REQUIRE(mixer.settings().channels.at(2) == ks::engine::ChannelSettings{});
    REQUIRE(mixer.settings().channels.at(3) == ks::engine::ChannelSettings{});
}

TEST_CASE("Mixer: global controls clamp at and past their bounds", "[mixer]") {
    const Interpolator interpolator;
    Mixer mixer{interpolator};
    const MixerSettings& settings = mixer.settings();

    mixer.set_crossfader(0.0);
    REQUIRE(settings.crossfader == 0.0);
    mixer.set_crossfader(1.0);
    REQUIRE(settings.crossfader == 1.0);
    mixer.set_crossfader(kInfinity);
    REQUIRE(settings.crossfader == 1.0);
    mixer.set_crossfader(-kInfinity);
    REQUIRE(settings.crossfader == 0.0);
    mixer.set_crossfader(0.75);
    REQUIRE(settings.crossfader == 0.75);

    mixer.set_crossfader_curve(kMinCrossfaderTransition);
    REQUIRE(settings.crossfader_transition == kMinCrossfaderTransition);
    mixer.set_crossfader_curve(0.005);
    REQUIRE(settings.crossfader_transition == kMinCrossfaderTransition);
    mixer.set_crossfader_curve(-kInfinity);
    REQUIRE(settings.crossfader_transition == kMinCrossfaderTransition);
    mixer.set_crossfader_curve(kInfinity);
    REQUIRE(settings.crossfader_transition == 1.0);
    mixer.set_crossfader_curve(0.2);
    REQUIRE(settings.crossfader_transition == 0.2);

    mixer.set_main_level(0.0);
    REQUIRE(settings.main_level_db == 0.0);
    mixer.set_main_level(kInfinity);
    REQUIRE(settings.main_level_db == 0.0);
    mixer.set_main_level(-kInfinity);
    REQUIRE(settings.main_level_db == -kInfinity);
    mixer.set_main_level(-6.0);
    REQUIRE(settings.main_level_db == -6.0);

    mixer.set_headphone_level(0.1);
    REQUIRE(settings.headphone_level_db == 0.0);
    mixer.set_headphone_level(kInfinity);
    REQUIRE(settings.headphone_level_db == 0.0);
    mixer.set_headphone_level(-kInfinity);
    REQUIRE(settings.headphone_level_db == -kInfinity);
    mixer.set_headphone_level(-3.0);
    REQUIRE(settings.headphone_level_db == -3.0);

    mixer.set_cue_mix(1.0);
    REQUIRE(settings.cue_mix == 1.0);
    mixer.set_cue_mix(kInfinity);
    REQUIRE(settings.cue_mix == 1.0);
    mixer.set_cue_mix(-kInfinity);
    REQUIRE(settings.cue_mix == 0.0);
    mixer.set_cue_mix(0.5);
    REQUIRE(settings.cue_mix == 0.5);
}

TEST_CASE("Mixer: NaN and out-of-range deck indices change nothing", "[mixer]") {
    const Interpolator interpolator;
    Mixer mixer{interpolator};
    mixer.set_trim(0, -3.0);
    mixer.set_fader(0, 0.5);
    mixer.set_crossfader(0.25);
    mixer.set_crossfader_curve(0.5);
    mixer.set_main_level(-1.0);
    mixer.set_headphone_level(-2.0);
    mixer.set_cue_mix(0.75);
    const MixerSettings before = mixer.settings();

    for (std::size_t deck = 0; deck < kDecks; ++deck) {
        mixer.set_trim(deck, kNaN);
        mixer.set_fader(deck, kNaN);
    }
    mixer.set_crossfader(kNaN);
    mixer.set_crossfader_curve(kNaN);
    mixer.set_main_level(kNaN);
    mixer.set_headphone_level(kNaN);
    mixer.set_cue_mix(kNaN);
    for (const std::size_t deck : {kDecks, kDecks + 1, std::numeric_limits<std::size_t>::max()}) {
        mixer.set_trim(deck, 3.0);
        mixer.set_fader(deck, 0.1);
        mixer.set_crossfader_assign(deck, CrossfaderSide::A);
        mixer.set_cue(deck, true);
    }
    REQUIRE(mixer.settings() == before);
}

TEST_CASE("Mixer: valid bus maps are accepted", "[mixer]") {
    const Interpolator interpolator;
    for (const BusMap map :
         {BusMap{.main = 0, .cue = 2}, BusMap{.main = 2, .cue = 0}, BusMap{.main = 4, .cue = 6},
          BusMap{.main = 6, .cue = 4}, BusMap{.main = 6, .cue = 0}}) {
        const Mixer constructed{interpolator, map};
        REQUIRE(constructed.settings().bus_map == map);

        Mixer mixer{interpolator};
        mixer.set_bus_map(map);
        REQUIRE(mixer.settings().bus_map == map);
    }
}

TEST_CASE("Mixer: invalid bus maps are refused", "[mixer]") {
    const Interpolator interpolator;
    const BusMap previous{.main = 4, .cue = 6};
    for (const BusMap map : {
             BusMap{.main = 1, .cue = 2}, // odd main
             BusMap{.main = 0, .cue = 3}, // odd cue
             BusMap{.main = 2, .cue = 2}, // equal
             BusMap{.main = 8, .cue = 2}, // main out of range
             BusMap{.main = 0, .cue = 8}, // cue out of range
             BusMap{.main = 7, .cue = 0}, // odd, and its pair is out of range
             BusMap{.main = 0, .cue = std::numeric_limits<std::size_t>::max() - 1},
         }) {
        const Mixer constructed{interpolator, map};
        REQUIRE(constructed.settings().bus_map == BusMap{});

        Mixer mixer{interpolator, previous};
        mixer.set_bus_map(map);
        REQUIRE(mixer.settings().bus_map == previous);
    }
}

} // namespace
