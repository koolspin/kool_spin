# Roadmap

Build a small, solid core first, and add features only on top of something already tested and measured. Each phase ends with exit criteria that are checked, not judged by feel.

- Stack and license: [ADR 0001](adr/0001-tech-stack.md).
- Reliability and process model: [ADR 0002](adr/0002-reliability-and-process-model.md). The music keeps playing through any failure short of a kernel panic or power loss.
- Engine threading, IPC, and memory: [ADR 0003](adr/0003-engine-threading-ipc-and-memory.md).
- Development workflow: `docs/process.md` (written in Phase 0). Each feature goes through spec, acceptance tests, plan, build loop, pull request, and review. Risky core code is built interactively; well-specified breadth work is built by Ralph loops.

Exit criteria are tagged `[auto]` (CI or a loop can verify it) or `[manual]` (needs the maintainer and real hardware). A build loop is done when all of its `[auto]` criteria pass.

## Test hardware

- Linux laptop (Ubuntu 24.04, PipeWire, x86_64) and a Mac.
- Hercules DJControl Inpulse 500: MIDI controller with a built-in audio interface (24-bit, 44.1 kHz; master on outputs 1–2, headphones on outputs 3–4). Mixxx ships a mapping for it (added in Mixxx 2.5.1).

## Phase 0: Foundations

Goal: a repo where AI-written code is checked automatically before a human reads it, and where build loops can run safely.

**Status:** complete on 2026-11-09. One manual check is still open: listening to `ks-tone` through JACK and on macOS (see the exit criteria).

**How it's built:** interactively. It creates the guardrails the loops depend on.

Build and checks:

- CMake presets: `debug`, `release`, `asan` (ASan + UBSan), `tsan`, `rtsan`, `fuzz` (libFuzzer).
- vcpkg manifest with a pinned baseline; Catch2 v3.
- `.clang-format` and `.clang-tidy`; warnings are errors in CI.
- `KS_NONBLOCKING` macro: `[[clang::nonblocking]]` where `__has_cpp_attribute` reports support, empty otherwise.
- GitHub Actions: Ubuntu 24.04 with Clang ≥ 20 (all presets); macOS arm64 with AppleClang (`debug`, `release`).
- Build and test commands recorded in `CLAUDE.md`.
- `LICENSE` file with the GPL-3.0 text.
- `ks-tone`: plays a sine wave through miniaudio.

Agent workflow:

- `docs/process.md`: the per-feature workflow, and which work is interactive versus looped.
- Templates in `specs/`: a feature spec (requirements, acceptance criteria tagged `[auto]`/`[manual]`, out of scope) and a plan.
- Loop runner: a bash script that starts a fresh `claude -p` process for each pass, so all state lives in files and git. Prompts are versioned in the repo as `PROMPT_plan.md` and `PROMPT_build.md`.
- Stop conditions: a maximum number of passes; all `[auto]` criteria passing; or the same failure three passes in a row, in which case the loop writes `BLOCKED.md` and exits.
- Protected paths (acceptance tests, golden files, sanitizer presets, ADRs): a Claude Code `PreToolUse` hook blocks loop edits to them, and a CI check fails any branch that changes them without the maintainer's approval.
- Loops run in a git worktree on a feature branch, never on `main`, with a narrow permission allowlist and the sandbox. The maintainer pushes and merges.

Exit criteria:

- [x] `[auto]` CI is green on Linux and macOS. *Every pull request since #3.*
- [x] `[auto]` A test that allocates inside a `KS_NONBLOCKING` function fails under the `rtsan` preset. This proves the guardrail fires; the test then becomes an expected failure. *`tests/guardrails/rtsan_canary.cpp` also covers locking a mutex.*
- [x] `[auto]` A loop's attempt to edit a protected file is blocked by the hook, and a branch that changes a protected file fails CI. *`tests/guardrails/protected_paths_test.sh`, a real `claude -p` run, and pull requests #4 and #5.*
- [ ] `[manual]` `ks-tone` plays on Linux (PipeWire through the JACK API, and ALSA) and on macOS. *Confirmed by the maintainer on 2026-11-09 on Linux with the PulseAudio and ALSA backends, including the Inpulse 500 via `--device`. Still open: JACK through PipeWire (needs `pipewire-jack`) and macOS (needs a Mac build environment).*
- [x] `[manual]` A trial loop on a small feature (such as `ks-tone` command-line options) runs end to end: it plans, builds, stops when its criteria pass, and writes a pull request description. A deliberately impossible task ends in `BLOCKED.md`. *`ks-tone` options: 1 plan pass and 8 build passes, $6.76, pull request #5. A spec requiring a new library ended in `BLOCKED.md` after one planning pass.*

## Phase 1: Headless engine and process model

Goal: glitch-free playback of real tracks, controlled from a CLI, that survives crashes of every other process and recovers from crashes of its own.

[ADR 0003](adr/0003-engine-threading-ipc-and-memory.md) decides the engine's threads, the shared-memory and socket protocols, and how tracks are held in memory. It also splits this phase in two:

- **Phase 1a** builds the engine as a library, in one process.
- **Phase 1b** separates the processes and adds crash recovery.

The shared-memory primitives have their final layouts from 1a on, so 1b changes what backs them, not the engine.

### Phase 1a: Engine library

**How it's built:** interactively for the shared-memory primitives, the render core and its drivers, the deck's interpolator, and the output safety stage. Loops for the mixer, the development harness, and the fuzz tests.

- Shared-memory primitives: the SPSC command ring, the seqlock state block, and the region they live in, backed by anonymous memory until 1b. The command decoder.
- `AudioBackend` interface, with a miniaudio implementation and an offline implementation for tests. The offline backend runs either as fast as possible (golden tests) or paced in real time while recording its output (chaos and soak tests). It can simulate device loss.
- Render core: one render function, called by one driver at a time. Device loss: a freewheel driver keeps the engine clock running on a timer, and the engine reattaches when the device returns.
- Deck: load (from a track already in memory), play/pause, seek, cue point, and variable rate including reverse, using an interpolating resampler. Track-to-device sample-rate conversion is folded into the same rate, so there is one interpolation stage.
- Mixer: N decks, channel gain, crossfader, a main bus and a headphone-cue bus, and mapping of buses to the channels of one device.
- Output safety stage: NaN/Inf to silence, limiter, denormal flushing, gain ramps.
- State out, including a visual play position (frame, host time, output latency) for the future waveform view. The resume state, and its validation.
- Development harness: drives the engine in one process from a script, playing synthetic signals or WAV files (through miniaudio's built-in decoder), either through a device or rendered to a file. It's for listening and soak tests, and isn't shipped.

Exit criteria:

- [ ] `[auto]` Offline-render golden tests for the deck and mixer: seek, rate change, reverse, crossfade. Golden files are compared with a tolerance. Property tests show the output doesn't depend on block size, bit for bit.
- [ ] `[auto]` Engine tests are clean under the `rtsan`, `tsan`, and `asan` presets. The command decoder and the resume-state validator are fuzzed under `fuzz`.
- [ ] `[auto]` Lock-free primitives pass stress tests on macOS arm64, whose weaker memory ordering exposes bugs that x86 hides.
- [ ] `[auto]` Simulated device loss keeps the clock running, and the engine resumes at the right position when the device returns.
- [ ] `[auto]` A corrupted resume state is rejected: the engine starts clean with decks paused, and never crashes.
- [ ] `[manual]` Pulling and reconnecting the Inpulse 500's USB cable mid-playback behaves like the simulated device loss.
- [ ] `[manual]` One-hour soak test through the harness on the Inpulse 500's audio interface: 4 decks, 128-frame buffer at 44.1 kHz (2.9 ms), scripted loads, seeks, and rate changes, with no buffer underruns caused by the engine.

### Phase 1b: Processes and crash recovery

**How it's built:** interactively for the shared-memory segments, the sockets, and the supervisor's restart logic. Loops for the decode worker, `ks-play`, and the fuzz tests.

- Supervisor, engine, and decode-worker processes, plus `ks-play`, a CLI client for manual testing. The supervisor is the hub: it creates every shared-memory segment and passes it on.
- Shared-memory segments (memfd on Linux, unlinked POSIX shared memory on macOS) and the Unix-socket control channel.
- Track loading: a worker decodes with FFmpeg into shared memory, which the engine maps read-only. Playback can start before decoding finishes. Track memory is locked when the OS permits. Tracks are capped at 2 GiB each, within a total memory budget.
- Engine restart: the engine publishes its state to shared memory. After a crash or a hang, the supervisor restarts it. It resumes with a fade-in, at the position the decks would have reached if they had kept playing. Crash-loop guard.
- Supervisor crash: the other processes keep running, and relaunching offers to stop the orphaned engine.
- Session journal, written by the supervisor. The previous session is restored, paused, at launch.
- Per-process logs through shared memory, with shared timestamps.
- Health checks reported by `ks-play`: real-time priority, memory locking, CPU power profile, battery. System sleep is blocked while decks are loaded.

Exit criteria:

- [ ] `[auto]` Tests are clean under the `rtsan`, `tsan`, and `asan` presets. Socket message parsing, the track segment header, and the session journal are fuzzed under `fuzz`.
- [ ] `[auto]` Killing `ks-play` or a decode worker with `kill -9` during playback leaves no gap in the recorded output.
- [ ] `[auto]` Killing the engine with `kill -9` resumes playback at the right position with a fade-in. Restart time is measured; target under 1 second.
- [ ] `[auto]` An engine whose audio thread stops making progress is killed and restarted, and resumes the same way as after a crash.
- [ ] `[auto]` Killing the supervisor with `kill -9` leaves playback running. Relaunching reports the running engine and can stop it.
- [ ] `[auto]` Corrupted shared state makes the engine start clean with decks paused; it never crashes.
- [ ] `[auto]` Deleting a playing track's file doesn't interrupt playback.
- [ ] `[auto]` After all processes are killed, relaunching restores the previous session with decks paused.
- [ ] `[manual]` Removing the USB drive that holds a playing track doesn't interrupt it.
- [ ] `[manual]` The one-hour soak test passes again with all processes running, driven through `ks-play`.
- [ ] `[manual]` After a reboot, launching restores the previous session with decks paused.

## Phase 2: Library and analysis

Goal: find any track instantly, and give every track a trustworthy beat grid.

**How it's built:** interactively for specs and acceptance tests. Loops for the schema and migrations, search, scanner, analysis workers, and BPM tuning against the protected evaluation set.

- Library component (Qt-free): SQLite schema with versioned migrations, in WAL mode with `synchronous=FULL`. Tables for tracks, analysis results, cue points, crates and playlists. One process owns the database: a CLI tool in this phase, the UI process from Phase 3.
- Rotating database backups at startup. Music files are never modified.
- Scanner: watched folders, a content fingerprint so moved or renamed files keep their analysis, and incremental rescans. TagLib metadata reading runs in worker processes.
- Full-text search with FTS5.
- Analysis in worker processes: beat grid and BPM (qm-dsp), 3-band waveform summaries, loudness (EBU R128). Results are stored with the analyzer version, so analyzer improvements trigger re-analysis.
- BPM evaluation harness on a labeled dataset, reporting the standard Accuracy1 (within 4% of the true tempo) and Accuracy2 (Accuracy1, but also accepting ×2, ×3, ½ and ⅓ octave errors). The dataset and scoring code are protected paths.

Exit criteria:

- [ ] `[auto]` Search over a synthetic 100k-track library returns in under 20 ms at p95.
- [ ] `[auto]` Rescanning an unchanged 10k-track library takes seconds, not minutes.
- [ ] `[auto]` A BPM accuracy baseline is recorded, and CI fails on regressions.
- [ ] `[auto]` A worker crashing on a malformed file flags that file, and the scan continues. Tag parsing is fuzzed.
- [ ] `[auto]` Killing the database-owning process mid-write leaves the database consistent.
- [ ] `[manual]` Scanning and analysis at full load cause no audio underruns during playback.

## Phase 3: Desktop UI

Goal: see the decks.

**How it's built:** interactively for the waveform renderer and its sync to the visual play position. Loops for the library browser and the other views.

- `ks-ui`: a Qt 6.8+ Qt Quick process that hosts the library component.
- Deck views: scrolling 3-band waveform rendered on the GPU (`QQuickRhiItem`), track overview, beat-grid overlay, transport controls.
- Library browser backed by the database.
- Health check shown before playing.

Exit criteria:

- [ ] `[auto]` Killing the UI with `kill -9` during playback causes no audio interruption. The UI restarts and shows the correct state.
- [ ] `[manual]` Waveforms render at the display refresh rate with 4 decks on a target laptop.
- [ ] `[manual]` The waveform playhead is latency-compensated: it shows what is audible, not what was just rendered.

## Phase 4: Controllers

Goal: play with real hardware, even without a screen.

**How it's built:** interactively for embedding QuickJS-ng (interrupt handling, error containment). Loops for the Mixxx control-name compatibility layer and the mapping loader.

- libremidi with hot-plug, in the engine process.
- Control bus using Mixxx's control names (`[Channel1]`, `play`, and so on).
- Mapping runtime on QuickJS-ng in the engine process, loading Mixxx XML and JS mappings unchanged; MIDI learn.
- Recorded Inpulse 500 MIDI sessions, replayed through a virtual MIDI port in tests.

Exit criteria:

- [ ] `[auto]` With the UI killed, recorded Inpulse 500 MIDI replayed through a virtual port still controls playback: play, cue, EQ, crossfader.
- [ ] `[auto]` A mapping script that throws or loops forever is stopped and logged; audio is unaffected.
- [ ] `[manual]` The Hercules DJControl Inpulse 500 works with Mixxx's stock mapping, unchanged.
- [ ] `[manual]` Unplugging and replugging the controller mid-set doesn't interrupt audio, and the mapping rebinds.

## Later (unordered)

Key lock (Rubber Band v3), sync and quantize, hot cues, loops, beat jump, effects, HID controllers, key detection, Ableton Link, rekordbox/Serato/Traktor library import, recording, stem separation, Windows (with ASIO and its own IPC layer), OS-level sandboxing of worker processes (seccomp or Landlock on Linux, sandbox profiles on macOS), and a dedicated library-database process if UI restarts prove disruptive.

Deferred by ADR 0003:

- headphone cue on a second audio device, with clock-drift compensation;
- a fallback audio device during device loss;
- a new supervisor adopting a running engine after a supervisor crash;
- tracks longer than the 2 GiB cap, held compressed in memory;
- native CoreAudio and JACK backends, if Phase 3 finds miniaudio's timing too coarse for the waveform playhead.

## Open design questions

None at the moment. The questions raised before Phase 1 (headphone cue on a second device, the shared-memory lifecycle, and the track memory cap) are settled in [ADR 0003](adr/0003-engine-threading-ipc-and-memory.md).
