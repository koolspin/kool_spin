// SPDX-License-Identifier: GPL-3.0-or-later
// The [manual] listening criterion in specs/mixer/spec.md: the mixer through
// the default device. Hidden, so it runs only when asked for:
//   KS_LISTEN_TRACK_A=a.wav KS_LISTEN_TRACK_B=b.wav ks_mixer_acceptance_tests "[listen]"
// Without them, it plays the synthetic tracks. The main bus goes to channels
// 1-2, and the headphones to 3-4 when the device has them.

#include <ks/engine/audio_backend.h>
#include <ks/engine/interpolator.h>
#include <ks/engine/miniaudio_backend.h>
#include <ks/engine/mixer.h>
#include <ks/engine/render_core.h>
#include <ks/engine/track.h>

#include "../deck/wav.h"
#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using ks::engine::test::MixerAction;
using ks::engine::test::MixerControl;

/// A track's samples and rate: the WAV file named by `variable`, or 30 s of
/// the synthetic track, starting `offset` frames in.
struct Loaded {
    std::vector<float> samples;
    std::uint32_t rate = ks::engine::test::kTrackRate;
    std::string name;
};

Loaded load_track(const char* variable, std::uint64_t offset) {
    Loaded loaded;
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing in the tests sets the environment
    if (const char* const path = std::getenv(variable); path != nullptr) {
        auto read = ks::engine::test::read_wav(path);
        CAPTURE(path);
        REQUIRE(read.has_value());
        ks::engine::test::WavAudio audio = std::move(read).value_or(ks::engine::test::WavAudio{});
        loaded.samples = std::move(audio.samples);
        loaded.rate = audio.sample_rate;
        loaded.name = path;
    } else {
        const std::uint64_t frames = 30ULL * loaded.rate;
        loaded.samples.reserve(frames * 2);
        for (std::uint64_t frame = 0; frame < frames; ++frame) {
            for (std::size_t channel = 0; channel < 2; ++channel) {
                loaded.samples.push_back(
                    ks::engine::test::synthetic(frame + offset, channel, loaded.rate));
            }
        }
        loaded.name = "the synthetic track";
    }
    loaded.samples.resize(
        std::min<std::size_t>(loaded.samples.size(), ks::engine::kMaxTrackFrames * 2));
    return loaded;
}

/// The listening script at `rate`, with a description of each section.
std::vector<std::pair<MixerControl, std::string>> listening_script(std::uint32_t rate) {
    using enum MixerAction;
    const auto at = [rate](double seconds) {
        return static_cast<std::uint64_t>(seconds * static_cast<double>(rate));
    };
    std::vector<std::pair<MixerControl, std::string>> script;
    const auto add = [&script](MixerControl control, std::string description = {}) {
        script.emplace_back(control, std::move(description));
    };
    // 0-10 s: a constant-power crossfade from A to B, in 100 steps.
    for (int i = 0; i <= 100; ++i) {
        add({.frame = at(0.1 * i), .action = Crossfader, .deck = 0, .value = 0.01 * i},
            i == 0 ? "crossfade from A to B over 10 s" : "");
    }
    // 10-16 s: cue A, sweeping the cue mix from cue to main; then cue B.
    add({.frame = at(10), .action = Cue, .deck = 0, .value = 1}, "cue A in the headphones");
    for (int i = 0; i <= 40; ++i) {
        add({.frame = at(10 + (0.1 * i)), .action = CueMix, .deck = 0, .value = 0.025 * i},
            i == 0 ? "sweep the cue mix from cue to main over 4 s" : "");
    }
    add({.frame = at(14), .action = Cue, .deck = 0, .value = 0}, "cue B instead, cue mix at cue");
    add({.frame = at(14), .action = Cue, .deck = 1, .value = 1});
    add({.frame = at(14), .action = CueMix, .deck = 0, .value = 0});
    add({.frame = at(16), .action = Cue, .deck = 1, .value = 0}, "cue off");
    // 16-22 s: ride B's fader down and back up.
    for (int i = 0; i <= 60; ++i) {
        const double position = i <= 30 ? 1.0 - (i / 30.0) : (i - 30) / 30.0;
        add({.frame = at(16 + (0.1 * i)), .action = Fader, .deck = 1, .value = position},
            i == 0 ? "ride B's fader down and up over 6 s" : "");
    }
    // 22-26 s: scratch cuts, every 125 ms.
    add({.frame = at(22), .action = Curve, .deck = 0, .value = 0.02},
        "scratch cuts with transition 0.02 for 4 s");
    for (int i = 0; i < 32; ++i) {
        add({.frame = at(22 + (0.125 * i)),
             .action = Crossfader,
             .deck = 0,
             .value = i % 2 == 0 ? 0.0 : 1.0});
    }
    std::ranges::stable_sort(script, {}, [](const auto& entry) { return entry.first.frame; });
    return script;
}

constexpr double kScriptSeconds = 26.0;

} // namespace

TEST_CASE("Listen: the mixer through the default device", "[.][listen]") {
    Loaded a = load_track("KS_LISTEN_TRACK_A", 0);
    Loaded b = load_track("KS_LISTEN_TRACK_B", 123'457);
    ks::engine::Track track_a{a.samples, a.rate};
    track_a.publish(a.samples.size() / 2, true);
    ks::engine::Track track_b{b.samples, b.rate};
    track_b.publish(b.samples.size() / 2, true);

    const ks::engine::Interpolator interpolator;
    ks::engine::Mixer mixer{interpolator};
    REQUIRE(mixer.deck(0).load(track_a));
    REQUIRE(mixer.deck(1).load(track_b));
    mixer.set_crossfader_assign(0, ks::engine::CrossfaderSide::A);
    mixer.set_crossfader_assign(1, ks::engine::CrossfaderSide::B);
    mixer.set_crossfader(0.0);
    mixer.deck(0).set_playing(true);
    mixer.deck(1).set_playing(true);

    // The script's frames depend on the device's rate, so the graph gets the
    // script once the device is open, before it starts.
    std::vector<MixerControl> controls;
    ks::engine::test::ScriptedMixer graph{mixer, {}};
    ks::engine::RenderCore core{graph};
    auto opened = ks::engine::open_miniaudio(
        core, ks::engine::MiniaudioConfig{.backends = ks::engine::MiniaudioBackends::System,
                                          .channels = 0,
                                          .sample_rate = 0,
                                          .period_frames = 128});
    REQUIRE(opened.has_value());
    ks::engine::AudioBackend& backend = **opened;
    const ks::engine::StreamInfo& info = backend.info();

    const auto script = listening_script(info.sample_rate);
    controls.reserve(script.size());
    for (const auto& [control, description] : script) {
        controls.push_back(control);
    }
    graph.set_script(controls);

    std::cout << std::format("A: {} ({} Hz)\nB: {} ({} Hz)\nOn {}: {}, {} Hz, {} channels{}\n",
                             a.name, a.rate, b.name, b.rate, info.backend, info.device,
                             info.sample_rate, info.channels,
                             info.channels >= 4 ? ", headphones on 3-4" : ", no headphone bus");
    for (const auto& [control, description] : script) {
        if (!description.empty()) {
            std::cout << std::format("  {:5.1f} s  {}\n",
                                     static_cast<double>(control.frame) / info.sample_rate,
                                     description);
        }
    }
    std::cout << std::format("  {:5.1f} s  stop\n", kScriptSeconds) << std::flush;

    REQUIRE(backend.start().has_value());
    std::this_thread::sleep_for(std::chrono::duration<double>{kScriptSeconds});
    core.set_muted(true);
    for (int i = 0; i < 200 && !core.silenced(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    backend.stop();
    REQUIRE(core.frames_rendered() > 0);
}
