// SPDX-License-Identifier: GPL-3.0-or-later
// The [manual] listening criterion in specs/deck/spec.md: a deck through the
// default device. Hidden, so it runs only when asked for:
//   KS_LISTEN_TRACK=track.wav ks_deck_acceptance_tests "[listen]"
// Without KS_LISTEN_TRACK, it plays the synthetic track.

#include <ks/engine/audio_backend.h>
#include <ks/engine/deck.h>
#include <ks/engine/interpolator.h>
#include <ks/engine/miniaudio_backend.h>
#include <ks/engine/render_core.h>
#include <ks/engine/track.h>

#include "test_support.h"
#include "wav.h"
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

using ks::engine::test::Action;
using ks::engine::test::Control;

/// The listening script, at a device rate of `device_rate`, for a track at
/// `track_rate`. Each entry's description is printed before playing.
std::vector<std::pair<Control, std::string>> listening_script(std::uint32_t device_rate,
                                                              std::uint32_t track_rate) {
    const auto at = [device_rate](double seconds) {
        return static_cast<std::uint64_t>(seconds * static_cast<double>(device_rate));
    };
    const auto track_frame = [track_rate](double seconds) {
        return seconds * static_cast<double>(track_rate);
    };
    std::vector<std::pair<Control, std::string>> script{
        {{.frame = at(0), .action = Action::Load, .value = 0}, "load"},
        {{.frame = at(0), .action = Action::Play, .value = 0}, "play at rate 1"},
        {{.frame = at(10), .action = Action::Rate, .value = 1.08}, "rate 1.08"},
        {{.frame = at(15), .action = Action::Rate, .value = 0.92}, "rate 0.92"},
        {{.frame = at(20), .action = Action::Rate, .value = 1.5}, "rate 1.5"},
        {{.frame = at(25), .action = Action::Rate, .value = -1.0}, "reverse"},
        {{.frame = at(28), .action = Action::Rate, .value = 1.0}, "rate 1"},
    };
    for (int i = 0; i < 6; ++i) {
        const double seconds = 28.0 + (0.5 * i);
        const bool pause = i % 2 == 0;
        script.push_back(
            {{.frame = at(seconds), .action = pause ? Action::Pause : Action::Play, .value = 0},
             pause ? "pause" : "play"});
    }
    for (int i = 0; i < 8; ++i) {
        const double seconds = 31.0 + (0.5 * i);
        const double to = i % 2 == 0 ? 20.0 : 24.0;
        script.push_back({{.frame = at(seconds), .action = Action::Seek, .value = track_frame(to)},
                          std::format("seek to {} s", to)});
    }
    script.push_back({{.frame = at(35), .action = Action::Rate, .value = 2.0}, "rate 2"});
    return script;
}

constexpr double kScriptSeconds = 38.0;

} // namespace

TEST_CASE("Listen: a deck through the default device", "[.][listen]") {
    // The track: the WAV file named by KS_LISTEN_TRACK, or 40 s of the
    // synthetic track at 44.1 kHz.
    std::vector<float> samples;
    std::uint32_t track_rate = ks::engine::test::kTrackRate;
    std::string name = "the synthetic track";
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing in the tests sets the environment
    if (const char* const path = std::getenv("KS_LISTEN_TRACK"); path != nullptr) {
        auto read = ks::engine::test::read_wav(path);
        CAPTURE(path);
        REQUIRE(read.has_value());
        ks::engine::test::WavAudio audio = std::move(read).value_or(ks::engine::test::WavAudio{});
        samples = std::move(audio.samples);
        track_rate = audio.sample_rate;
        name = path;
    } else {
        const std::uint64_t frames = 40ULL * track_rate;
        samples.reserve(frames * 2);
        for (std::uint64_t frame = 0; frame < frames; ++frame) {
            samples.push_back(ks::engine::test::synthetic(frame, 0, track_rate));
            samples.push_back(ks::engine::test::synthetic(frame, 1, track_rate));
        }
    }
    samples.resize(std::min<std::size_t>(samples.size(), ks::engine::kMaxTrackFrames * 2));
    ks::engine::Track track{samples, track_rate};
    track.publish(samples.size() / 2, true);

    const ks::engine::Interpolator interpolator;
    ks::engine::Deck deck{interpolator};
    ks::engine::test::ScriptGraph graph{deck, track, {}};
    ks::engine::RenderCore core{graph};
    auto opened = ks::engine::open_miniaudio(
        core, ks::engine::MiniaudioConfig{.backends = ks::engine::MiniaudioBackends::System,
                                          .channels = 0,
                                          .sample_rate = 0,
                                          .period_frames = 128});
    REQUIRE(opened.has_value());
    ks::engine::AudioBackend& backend = **opened;
    const ks::engine::StreamInfo& info = backend.info();

    const auto script = listening_script(info.sample_rate, track_rate);
    std::vector<Control> controls;
    controls.reserve(script.size());
    for (const auto& [control, description] : script) {
        controls.push_back(control);
    }
    graph.set_script(controls);

    std::cout << std::format("Playing {} ({} Hz, {:.1f} s) on {}: {}, {} Hz, {} channels, "
                             "{}-frame period\n",
                             name, track_rate,
                             static_cast<double>(samples.size()) / 2.0 / track_rate, info.backend,
                             info.device, info.sample_rate, info.channels, info.period_frames);
    for (const auto& [control, description] : script) {
        std::cout << std::format("  {:5.1f} s  {}\n",
                                 static_cast<double>(control.frame) / info.sample_rate,
                                 description);
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
