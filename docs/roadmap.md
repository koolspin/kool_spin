# Roadmap

Build a small, solid core first, and add features only on top of something already tested and measured. Each phase ends with exit criteria that are checked, not judged by feel.

Stack and license: [ADR 0001](adr/0001-tech-stack.md).

## Test hardware

- Linux laptop (Ubuntu 24.04, PipeWire) and a Mac.
- Hercules DJControl Inpulse 500: MIDI controller with a built-in audio interface (24-bit, 44.1 kHz; master on outputs 1–2, headphones on outputs 3–4). Mixxx ships a mapping for it (added in Mixxx 2.5.1).

## Phase 0: Foundations

Goal: a repo where AI-written code is checked automatically before a human reads it.

- CMake presets: `debug`, `release`, `asan` (ASan + UBSan), `tsan`, `rtsan`.
- vcpkg manifest with a pinned baseline; Catch2 v3.
- `.clang-format` and `.clang-tidy`; warnings are errors in CI.
- `KS_NONBLOCKING` macro: `[[clang::nonblocking]]` where `__has_cpp_attribute` reports support, empty otherwise.
- GitHub Actions: Ubuntu 24.04 with Clang ≥ 20 (all presets); macOS arm64 with AppleClang (`debug`, `release`).
- Build and test commands recorded in `CLAUDE.md`.
- `LICENSE` file with the GPL-3.0 text.
- `ks-tone`: plays a sine wave through miniaudio.

Exit criteria:

- [ ] CI is green on Linux and macOS.
- [ ] A test that allocates inside a `KS_NONBLOCKING` function fails under the `rtsan` preset. This proves the guardrail fires; the test then becomes an expected failure.
- [ ] `ks-tone` plays on Linux (PipeWire through the JACK API, and ALSA) and on macOS.

## Phase 1: Headless playback engine

Goal: glitch-free playback of real tracks, controlled from a CLI, with no UI.

- `AudioBackend` interface, with a miniaudio implementation and an offline implementation for tests.
- Track loader: FFmpeg decodes to float32 in memory, in chunks, on a loader thread. Playback can start before decoding finishes.
- Deck: load, play/pause, seek, cue point, and variable rate including reverse, using an interpolating resampler. Track-to-device sample-rate conversion is folded into the same rate, so there is one interpolation stage.
- Mixer: N decks, channel gain, crossfader, a main bus and a headphone-cue bus, and mapping of buses to device channels.
- Control path: commands go in through a lock-free queue. State comes out through atomics and seqlock snapshots, including a visual play position (frame, host time, output latency) for the future waveform view.
- `ks-play`: a CLI player for manual testing.

Exit criteria:

- [ ] Offline-render golden tests for the deck and mixer: seek, rate change, reverse, crossfade.
- [ ] Engine tests are clean under the `rtsan`, `tsan`, and `asan` presets.
- [ ] One-hour soak test on the Inpulse 500's audio interface: 4 decks, 128-frame buffer at 44.1 kHz (2.9 ms), scripted loads, seeks, and rate changes, with no buffer underruns caused by the engine.
- [ ] Unplugging the audio device mid-playback stops cleanly, and playback recovers when the device returns.

## Phase 2: Library and analysis

Goal: find any track instantly, and give every track a trustworthy beat grid.

- SQLite schema (WAL mode) with versioned migrations: tracks, analysis results, cue points, crates and playlists.
- Scanner: watched folders, TagLib metadata, a content fingerprint so moved or renamed files keep their analysis, and incremental rescans.
- Full-text search with FTS5.
- Analysis worker pool: beat grid and BPM (qm-dsp), 3-band waveform summaries, loudness (EBU R128). Results are stored with the analyzer version, so analyzer improvements trigger re-analysis.
- BPM evaluation harness on a labeled dataset, reporting the standard Accuracy1 (within 4% of the true tempo) and Accuracy2 (Accuracy1, but also accepting ×2, ×3, ½ and ⅓ octave errors).

Exit criteria:

- [ ] Search over a synthetic 100k-track library returns in under 20 ms at p95.
- [ ] Rescanning an unchanged 10k-track library takes seconds, not minutes.
- [ ] A BPM accuracy baseline is recorded, and CI fails on regressions.
- [ ] Scanning and analysis at full load cause no audio underruns during playback.

## Phase 3: Desktop UI

Goal: see the decks.

- Qt 6.8+ Qt Quick application shell. The engine stays Qt-free.
- Deck views: scrolling 3-band waveform rendered on the GPU (`QQuickRhiItem`), track overview, beat-grid overlay, transport controls.
- Library browser backed by the database.

Exit criteria:

- [ ] Waveforms render at the display refresh rate with 4 decks on a target laptop.
- [ ] The waveform playhead is latency-compensated: it shows what is audible, not what was just rendered.
- [ ] Deliberately stalling the UI thread causes no audio dropouts.

## Phase 4: Controllers

Goal: play with real hardware.

- libremidi with hot-plug.
- Control bus using Mixxx's control names (`[Channel1]`, `play`, and so on).
- Mapping runtime on QJSEngine that loads Mixxx XML and JS mappings unchanged; MIDI learn.

Exit criteria:

- [ ] The Hercules DJControl Inpulse 500 works with Mixxx's stock mapping, unchanged.
- [ ] Unplugging and replugging a controller mid-set doesn't interrupt audio.

## Later (unordered)

Key lock (Rubber Band v3), sync and quantize, hot cues, loops, beat jump, effects, HID controllers, key detection, Ableton Link, rekordbox/Serato/Traktor library import, recording, stem separation, Windows with ASIO.

## Open design questions

These become ADR 0002 (engine threading and memory model) once settled.

- **Track memory.** Keep each decoded track entirely in RAM, or use a windowed chunk cache? Whole-track residency is simple and makes scratching, reverse, and seeking trivial, at about 21 MB per minute of 44.1 kHz stereo float32. A windowed cache bounds memory and handles two-hour mix files. *Proposal:* whole-track residency with a size cap; add windowing when long files need it.
- **Headphone cue on a second device.** Driving main and cue from two separate audio devices needs clock-drift compensation. *Proposal:* Phase 1 supports main and cue on one multi-channel device only, which covers most DJ controllers with built-in audio, including the Inpulse 500 (outputs 1–2 and 3–4).
