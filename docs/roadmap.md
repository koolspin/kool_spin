# Roadmap

Build a small, solid core first, and add features only on top of something already tested and measured. Each phase ends with exit criteria that are checked, not judged by feel.

- Stack and license: [ADR 0001](adr/0001-tech-stack.md).
- Reliability and process model: [ADR 0002](adr/0002-reliability-and-process-model.md). The music keeps playing through any failure short of a kernel panic or power loss.

## Test hardware

- Linux laptop (Ubuntu 24.04, PipeWire) and a Mac.
- Hercules DJControl Inpulse 500: MIDI controller with a built-in audio interface (24-bit, 44.1 kHz; master on outputs 1–2, headphones on outputs 3–4). Mixxx ships a mapping for it (added in Mixxx 2.5.1).

## Phase 0: Foundations

Goal: a repo where AI-written code is checked automatically before a human reads it.

- CMake presets: `debug`, `release`, `asan` (ASan + UBSan), `tsan`, `rtsan`, `fuzz` (libFuzzer).
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

## Phase 1: Headless engine and process model

Goal: glitch-free playback of real tracks, controlled from a CLI, that survives crashes of every other process and recovers from crashes of its own.

Processes and IPC:

- Supervisor, engine, and decode-worker processes, plus `ks-play`, a CLI client for manual testing.
- Shared-memory primitives: SPSC command ring, seqlock state block, track buffer regions. A Unix-socket control channel.
- Engine restart: the engine publishes its state to shared memory. After a crash, the supervisor restarts it and it resumes with a fade-in. Crash-loop guard.
- Session journal, written by the supervisor. The previous session is restored, paused, at launch.

Audio:

- `AudioBackend` interface, with a miniaudio implementation and an offline implementation for tests. The offline backend runs either as fast as possible (golden tests) or paced in real time while recording its output (chaos and soak tests).
- Device loss: the engine clock keeps running on a timer, and the engine reattaches when the device returns.
- Track loading: a worker decodes with FFmpeg into shared memory, which the engine maps read-only. Playback can start before decoding finishes. Track memory is locked when the OS permits.
- Deck: load, play/pause, seek, cue point, and variable rate including reverse, using an interpolating resampler. Track-to-device sample-rate conversion is folded into the same rate, so there is one interpolation stage.
- Mixer: N decks, channel gain, crossfader, a main bus and a headphone-cue bus, and mapping of buses to device channels.
- Output safety stage: NaN/Inf to silence, limiter, denormal flushing, gain ramps.
- State out, including a visual play position (frame, host time, output latency) for the future waveform view.
- Health checks reported by `ks-play`: real-time priority, memory locking, CPU power profile, battery. System sleep is blocked while decks are loaded.

Exit criteria:

- [ ] Offline-render golden tests for the deck and mixer: seek, rate change, reverse, crossfade.
- [ ] Engine tests are clean under the `rtsan`, `tsan`, and `asan` presets. Shared-memory readers and IPC message parsing are fuzzed under `fuzz`.
- [ ] Killing `ks-play` or a decode worker with `kill -9` during playback leaves no gap in the recorded output.
- [ ] Killing the engine with `kill -9` resumes playback at the right position with a fade-in. Restart time is measured; target under 1 second.
- [ ] Corrupted shared state makes the engine start clean with decks paused; it never crashes.
- [ ] Unplugging the audio device mid-playback keeps the clock running, and plugging it back in resumes at the right position.
- [ ] Removing the drive that holds a playing track doesn't interrupt it.
- [ ] One-hour soak test on the Inpulse 500's audio interface: 4 decks, 128-frame buffer at 44.1 kHz (2.9 ms), scripted loads, seeks, and rate changes, with no buffer underruns caused by the engine.
- [ ] After a reboot, launching restores the previous session with decks paused.

## Phase 2: Library and analysis

Goal: find any track instantly, and give every track a trustworthy beat grid.

- Library component (Qt-free): SQLite schema with versioned migrations, in WAL mode with `synchronous=FULL`. Tables for tracks, analysis results, cue points, crates and playlists. One process owns the database: a CLI tool in this phase, the UI process from Phase 3.
- Rotating database backups at startup. Music files are never modified.
- Scanner: watched folders, a content fingerprint so moved or renamed files keep their analysis, and incremental rescans. TagLib metadata reading runs in worker processes.
- Full-text search with FTS5.
- Analysis in worker processes: beat grid and BPM (qm-dsp), 3-band waveform summaries, loudness (EBU R128). Results are stored with the analyzer version, so analyzer improvements trigger re-analysis.
- BPM evaluation harness on a labeled dataset, reporting the standard Accuracy1 (within 4% of the true tempo) and Accuracy2 (Accuracy1, but also accepting ×2, ×3, ½ and ⅓ octave errors).

Exit criteria:

- [ ] Search over a synthetic 100k-track library returns in under 20 ms at p95.
- [ ] Rescanning an unchanged 10k-track library takes seconds, not minutes.
- [ ] A BPM accuracy baseline is recorded, and CI fails on regressions.
- [ ] Scanning and analysis at full load cause no audio underruns during playback.
- [ ] A worker crashing on a malformed file flags that file, and the scan continues. Tag parsing is fuzzed.
- [ ] Killing the database-owning process mid-write leaves the database consistent.

## Phase 3: Desktop UI

Goal: see the decks.

- `ks-ui`: a Qt 6.8+ Qt Quick process that hosts the library component.
- Deck views: scrolling 3-band waveform rendered on the GPU (`QQuickRhiItem`), track overview, beat-grid overlay, transport controls.
- Library browser backed by the database.
- Health check shown before playing.

Exit criteria:

- [ ] Waveforms render at the display refresh rate with 4 decks on a target laptop.
- [ ] The waveform playhead is latency-compensated: it shows what is audible, not what was just rendered.
- [ ] Killing the UI with `kill -9` during playback causes no audio interruption. The UI restarts and shows the correct state.

## Phase 4: Controllers

Goal: play with real hardware, even without a screen.

- libremidi with hot-plug, in the engine process.
- Control bus using Mixxx's control names (`[Channel1]`, `play`, and so on).
- Mapping runtime on QuickJS-ng in the engine process, loading Mixxx XML and JS mappings unchanged; MIDI learn.

Exit criteria:

- [ ] The Hercules DJControl Inpulse 500 works with Mixxx's stock mapping, unchanged.
- [ ] With the UI killed, the controller still controls playback: play, cue, EQ, crossfader.
- [ ] Unplugging and replugging the controller mid-set doesn't interrupt audio, and the mapping rebinds.
- [ ] A mapping script that throws or loops forever is stopped and logged; audio is unaffected.

## Later (unordered)

Key lock (Rubber Band v3), sync and quantize, hot cues, loops, beat jump, effects, HID controllers, key detection, Ableton Link, rekordbox/Serato/Traktor library import, recording, stem separation, Windows (with ASIO and its own IPC layer), OS-level sandboxing of worker processes (seccomp or Landlock on Linux, sandbox profiles on macOS), and a dedicated library-database process if UI restarts prove disruptive.

## Open design questions

The details become ADR 0003 (engine threading, IPC, and memory) during Phase 1.

- **Headphone cue on a second device.** Driving main and cue from two separate audio devices needs clock-drift compensation. *Proposal:* Phase 1 supports main and cue on one multi-channel device only, which covers most DJ controllers with built-in audio, including the Inpulse 500 (outputs 1–2 and 3–4).
- **Shared-memory lifecycle.** How segments are named, sized, passed between processes, and cleaned up after an unclean exit, including stale segments left by a crashed supervisor. Linux offers `memfd_create`; macOS only has POSIX `shm_open`, with a 31-character name limit.
- **Track memory cap.** Whole-track residency is decided (ADR 0002). The per-track size limit, and what happens with longer files (refuse them, or decode in windows), are not.
