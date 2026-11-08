# ADR 0002: Reliability and process model

- **Status:** Accepted
- **Date:** 2026-11-08

## Context

The maintainer's DJ software from the 1990s lost sets when Windows kernel audio drivers crashed and took the whole machine down. kool_spin's top requirement is that this never happens again, in any form:

> The music keeps playing through any failure short of a kernel panic or power loss. After those, the previous session is back within seconds.

**What we can't control: kernel drivers.** No user-space program survives a kernel crash. We reduce the exposure through platform and hardware choice:

- **macOS:** on Apple Silicon, loading a third-party kernel extension requires lowering system security, so audio vendors ship user-space drivers (AudioDriverKit, Audio Server plug-ins). Class-compliant USB devices use Apple's own driver.
- **Linux:** USB audio and MIDI use the kernel's in-tree class driver (`snd-usb-audio`), not vendor code.
- **Windows (later):** prefer class-compliant devices on Microsoft's in-box USB Audio 2.0 driver. Vendor ASIO devices usually ship their own kernel drivers.

**What we can control: everything else.** The realistic ways a user-space DJ app loses a set are:

- crashes in the UI or the GPU driver;
- decoder crashes on malformed files;
- bugs in our own code;
- audio or MIDI devices disconnecting;
- the drive holding the music disappearing;
- the OS sleeping or throttling;
- corrupted data.

**Prior art.**

- Mainstream DJ apps run as a single process. Serato's support documentation says audio stops immediately when the app crashes, and Mixxx is a single process.
- Dorico runs its audio engine as a separate process, but quits when that process dies. Splitting processes alone doesn't keep audio going.
- Bitwig isolates plugins in separate processes, and audio usually survives a plugin crash.
- JACK and PipeWire run real-time audio graphs across processes through shared memory.
- Ardour's developers show why process boundaries inside the per-buffer audio path are too expensive: hundreds of context switches per audio block, at about 30 µs each.

## Decision

### 1. Separate processes, with recovery

| Process | Contains | Allowed dependencies | If it crashes |
|---|---|---|---|
| **Supervisor** (`kool_spin`) | Starts, monitors, and restarts the other processes. Creates and owns the shared-memory regions, so they outlive any child. Writes the session journal. | Standard library only | The children keep running. Automatic recovery is unavailable until the next launch, and the UI says so. |
| **Engine** (`ks-engine`) | Audio device I/O, decks, mixer, output safety stage, MIDI I/O, controller mapping scripts. | miniaudio, libremidi, QuickJS-ng; later, a time-stretch library | The supervisor restarts it. It maps the decoded tracks and its last published state from shared memory, and resumes playing with a short fade-in. |
| **UI** (`ks-ui`) | Qt Quick interface, GPU waveforms. Hosts the library component, which owns the database. | Qt, SQLite | Audio and controllers are unaffected. The supervisor restarts it, and it rebuilds its view from the engine's state. |
| **Workers** (`ks-worker`) | Decoding, tag reading, analysis. One job at a time; disposable. | FFmpeg, TagLib, qm-dsp, libebur128 | That job fails with an error, and the file is flagged so it isn't retried in a loop. Nothing else is affected. |

Adding a dependency to the engine or the supervisor requires updating this ADR.

A crash-loop guard stops restarting a process that fails repeatedly in a short window. For the engine, the guard makes one final restart with decks paused and the saved state discarded.

### 2. No process boundary in the audio path

The whole real-time path runs inside the engine process: device callback, decks, mixer, safety stage, and MIDI input. The engine never waits on another process.

- **Commands in:** lock-free single-producer/single-consumer rings in shared memory, one per client. The audio thread reads them without system calls.
- **State out:** seqlock-protected blocks in shared memory: deck positions, the latency-compensated visual play position, levels.
- **Bulk data:** decoded tracks and waveform summaries in shared-memory regions. Workers write them; the engine maps them read-only.
- **Non-real-time control:** Unix domain sockets, for handshakes, job requests, and errors.

The engine treats everything it reads from shared memory as untrusted input, and validates indices, lengths, and enum values before use. A buggy or crashed client can't crash it.

### 3. Playback never touches the disk

Tracks are fully decoded into memory, and the engine never reads files. Playback can start while decoding continues. Track memory is locked (`mlock`) when the OS permits, so it can't be paged out. Removing the drive that holds the music doesn't interrupt tracks already loaded.

### 4. Device loss is normal operation

If the audio device disappears, the engine keeps its clock running on a system timer and the decks keep advancing, the way a turntable keeps spinning when a cable is pulled. When the device returns, the engine reattaches automatically at the current position. An optional fallback device can take over in the meantime.

MIDI devices are reattached, and their mappings rebound, the same way.

### 5. Output safety stage

The last stage before the device is always on:

- NaN and Inf samples become silence, and are counted.
- A limiter caps the output level.
- Denormals are flushed (FTZ/DAZ on the audio thread).
- Gain ramps smoothly on start, stop, device change, and engine restart.

A bug must never become a full-scale blast through a PA.

### 6. Data safety

- Users' music files are never modified unless they opt in.
- One process owns the library database. SQLite runs in WAL mode with `synchronous=FULL`, and bulk writes are batched.
- Rotating database backups are made at startup.
- The session journal (loaded tracks, positions, cue points, mixer state) is written atomically at least once a second. After an engine crash, the engine resumes playing. After an app or system restart, the session is restored with decks paused.

### 7. Environment checks and power management

- A health check before playing warns about:
  - real-time scheduling priority being unavailable;
  - memory locking not being permitted;
  - a power-saving CPU profile, or running on battery;
  - audio callback load too close to the buffer deadline.
- System sleep, display sleep, and macOS App Nap are blocked while any deck is loaded.

### 8. Verification

Each mechanism above has an automated test:

- **Chaos tests**, using an offline audio backend paced in real time that records its output:
  - `kill -9` a client or worker during playback, and assert the recorded audio has no gap;
  - kill the engine, and assert it resumes at the right position;
  - corrupt the shared state, and assert the engine starts clean.
- **Fuzzing** (libFuzzer) for everything that parses outside input: shared-memory readers, IPC messages, mapping files, tags, library imports, and the decode path.
- **Soak tests** on real hardware (see the [roadmap](../roadmap.md)).

A crash handler in each process records a backtrace and recent log lines.

## Consequences

- Phase 1 grows: the supervisor, engine, a decode worker, the IPC layer, and a CLI client are needed from the start.
- Debugging spans processes. We need launch scripts that start any process under a debugger, and per-process logs with shared timestamps.
- Process lifecycle bugs (orphaned processes, stale shared memory, restart loops) become a new failure class. The chaos tests exist to catch them.
- Windows will need its own shared-memory and IPC implementation behind the same interfaces.
- Whole-track residency and locked memory raise RAM use: about 21 MB per minute of loaded 44.1 kHz stereo audio.
- Controller scripting moves from QJSEngine to QuickJS-ng. Mixxx mappings are written for QJSEngine, so compatibility has to be verified.

## Alternatives considered

- **Single process with careful threading** (the Mixxx model): simpler, but any crash anywhere stops the music.
- **Engine/UI split without supervision** (the Dorico model): splitting alone doesn't keep the music playing when the engine dies.
- **Process boundaries inside the audio path** (sandboxed effects or plugins): rejected for the core path, per Ardour's analysis. Could be revisited for third-party plugins, if we ever host them.
- **A dedicated library-database process:** deferred. If UI restarts prove disruptive, the database can move into its own process.
