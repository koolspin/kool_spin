# Feature: Engine restart (step 7a)

- **Status:** Approved by the maintainer on 2026-11-19. Protected: build loops can't change it.
- **Roadmap:** Phase 1b, step 7a
- **Built by:** interactive

## Goal

When `ks-engine` ends without being told to, the supervisor starts a new one, which carries on from where the old one stopped: the same state segment, the same log, every track the old engine held, and every client's command ring, at the same slot. The new engine resumes its decks from the resume state, moved on by the time that passed, and fades in over 50 ms. A crash-loop guard keeps a failing engine from restarting forever. A recording segment, which outlives the engine, lets chaos tests check the output across a `kill -9`.

Step 7b (a separate spec) adds hang detection: the heartbeat watch and the pings.

## Context

- **ADRs that apply:**
  - **ADR 0002:** the supervisor restarts a crashed engine, which maps the decoded tracks and its last published state and resumes with a short fade-in. A crash-loop guard makes the last restart start with decks paused and the saved state discarded. Chaos tests use a paced offline backend that records its output.
  - **ADR 0003 §11:** two resume slots, the newest valid one wins; the new engine gets every segment again; a deck whose track isn't among those passed comes back empty; rings' tails are read back and range-checked; playing decks move on by the time since the snapshot; restart time target under 1 second. §8: copy, validate, use.
  - **ADR 0003 §1:** the paced backend, with or without recording.
- **What exists:**
  - **Engine library** (`specs/resume-state/spec.md`): `Engine(state, tracks, map)` resumes from the newest valid slot and gives `resume_outcome()`.
  - **`kool_spin`** (`specs/processes/spec.md` requirements 15 and 20): starts `ks-engine` with the socket at descriptor 3, the state segment at 4, and the log segment at 5. Today an engine that ends unexpectedly ends the supervisor with status 3. The supervisor keeps every client's ring segment (`Ring::segment`) and every loaded track's (`Load::segment`), so it can pass them again.
  - **Rings, tracks, logs** (`specs/client-rings/spec.md`, `specs/track-loading/spec.md`, `specs/logs/spec.md`): each of those specs defers "giving a restarted engine X again" to this step. The logs spec says a restarted engine's lines go on in the same file.
- **The maintainer's design choices** (2026-11-19):
  - step 7 is split: this spec is 7a, restart after an engine exit, plus the recording segment; 7b is hang detection;
  - a restarted engine gets its tracks as inherited descriptors named on its command line, so they're mapped before the resuming `Engine` is built and the device opens. Rings are attached again by the existing `Attach` message, after `Ready`;
  - chaos tests record through a new paced backend in `ks-engine` and a recording segment the supervisor creates.
- **This spec extends** `specs/processes/spec.md` requirements 15, 20, and 21 (an engine ending unexpectedly is a restart, not an exit); `specs/client-rings/spec.md` requirement 10; and `specs/track-loading/spec.md` requirements 11 and 17. Everything else in them still applies.

## Requirements

### Restarting

1. **When it restarts:** an engine process that ends while the supervisor is in `Running` (any exit status, any signal) is restarted. An engine that ends before its first `Ready` still ends the supervisor with status 1, as before: there is no session to carry on. While `Stopping` or `Terminating`, nothing changes.
2. **The sequence.** The supervisor:
   1. reaps the process, drains its log, and reports how it ended (`ks-engine <how>`, then `restarting ks-engine (restart n)`);
   2. answers every request still waiting for the dead engine, as requirement 8 says, and closes its socket;
   3. starts a new `ks-engine` as at launch (same engine path and arguments), with a new socket pair, **the same state segment** and **the same log segment**, the recording segment if there is one (requirement 14), and the tracks of requirement 4;
   4. waits up to `kEngineReadyTimeout` for its `Ready`;
   5. on `Ready`, sends an `Attach` for every client's ring (requirement 6).

   Clients' connections stay open throughout. The listening socket stays open too, so a client can connect during a restart, and is told what requirement 9 says.
3. **The new engine's lock:** the old engine's `engine` lock is released when it dies; the new engine takes it as at launch. A lock still held by a live process (an engine that didn't die) is a failed start, counted as in requirement 11.
4. **Tracks.** The supervisor passes the segment of every `Load` whose engine state is `Waiting` or `Holds`, at descriptors 7 and up in the order of their IDs, each named on the command line as `--track <id>:<fd>`. At most `kRestartTracks` (16) are passed; there can't be more (4 decks, a main and a cue head each, and tracks being replaced).
5. **`ks-engine --track <id>:<fd>`.** The engine adopts each descriptor, maps it read-only as a track segment (taking the size from `fstat`), validates the header (a track segment's header has no ID, so the engine takes `<id>` on the supervisor's word), and takes the rate from it. A track that fails any check is reported in the log and skipped: its deck comes back empty. Repeating an ID or a descriptor, or giving an ID of 0, is a usage error (status 2). The engine then builds `Engine(state, tracks)`, **before** it opens the device, so decks resume with the track already in memory. Without `--clean` (requirement 11) the engine resumes; with it, it builds the engine as at launch and ignores `--track`.
6. **Rings.** `Attach` gains `slot` (u32, below `kStateClients`): the slot the ring must take. The engine attaches it there, reads the ring's tail back from the segment and checks `head − tail` is within the capacity (ADR 0003 §8; a ring that fails the check is reset), and answers `Attached` with the same slot. A slot that's in use is refused with `Unavailable`. For a new client's ring, the supervisor picks the lowest free slot itself and sends it. Commands the client wrote while the engine was away are applied, in order, once the ring is attached. Commands carry absolute values, so one applied twice does no harm.
7. **Unreleased tracks.** After it resumes, the engine sends `TrackReleased` for every track it was given that no deck holds (for example, one whose `LoadTrack` was in flight when the old engine died), exactly as if a deck had dropped it. The supervisor closes it as usual. A track whose deck holds it is released later as before.
8. **Requests waiting for the dead engine.** Each is answered with an `Error` of code `Unavailable`, text starting `ks-engine restarted`:
   - a `LoadFile`'s `FileLoaded` that hadn't come (the load ends; its worker is stopped as for any failed load, and its track counts as `Done`);
   - an `OpenRing` waiting for `Attached`, an `UnloadDeck` waiting for `DeckUnloaded`.

   A `Detach` waiting for `Detached` is simply done: the ring is gone. A track whose `LoadTrack` was answered `TrackLoaded` stays with its deck; `DecodeEnded` still reaches the client.
9. **Requests during a restart** (from step 2.2 until every `Attach` is answered): `LoadFile`, `OpenRing`, and `UnloadDeck` are answered `Unavailable`, text `ks-engine is restarting`. A client can ask again. `Ping`, `Stop`, and `Hello` are answered as usual. A `Stop` while the new engine is starting stops it as `Stopping` does today (the 2 s limit, then `SIGKILL`).
10. **Workers keep decoding.** A worker still decoding when the engine dies carries on, and finishes its track's header as usual. The new engine reads the same header, so a track that was still decoding resumes waiting at its limit, then plays on.
11. **The crash-loop guard** (ADR 0002, ADR 0003 §11). The supervisor counts engine exits in a sliding window of `kCrashLoopWindow` (30 s): an exit after `Ready`, a failure to start, a `Ready` that doesn't come in time (the new engine is killed with `SIGKILL`), or any other end. Each counts once.
    - The first and second exits in the window restart as requirement 2 says.
    - The third makes a **clean restart**: the supervisor zero-fills the engine-state segment past its header, passes no tracks, and starts the engine with `--clean`. Decks come back empty and paused, and no ring's slot is changed. The clients' rings are attached again as usual.
    - A fourth exit in the window gives up: the supervisor reports `ks-engine keeps failing; not restarting it`, closes the clients, stops its workers, and exits with status 3.
    - Exits more than 30 s apart don't count together. After a clean restart, the next exit that is more than 30 s later counts as the first again.
12. **Restart time.** The supervisor logs `ks-engine restarted in <n> ms` once the new engine's `Ready` has come: n is the time from reaping the old process to `Ready`.
13. **Log.** The new engine's log writer carries on from the segment's state: its lines go on in the same segment, in order after the old engine's, with the same timestamps. (This is the logs spec's "lines go on in the same file".)

### The recording segment

14. **`kool_spin --recording`** creates a recording segment (`SegmentKind::Recording`, layout version 1, `kRecordingFrames` frames: 2²² of stereo `float`, about 95 s at 44.1 kHz, a ring that keeps the newest) and passes it to every engine it starts at descriptor 6, named `--record-fd 6` (tracks start at 7, requirement 4). Without `--recording` no segment exists, and `OpenRecording` is refused with `NotSupported`.
15. **The layout.** After the header: `frames_written` (u64, total frames ever recorded, release-stored by the audio thread after each period's frames), `starts` (u64, how many engines have started on the segment), `boundaries` (`kMaxBoundaries` = 64 entries of `frame` and `host_time`, one per engine start: where in `frames_written` the engine started, and its host time), then the frames. A new engine continues `frames_written`, writes `starts + 1`'s boundary, and records the output the safety stage hands the device, period after period. Zero-filled memory is valid. Readers copy, then re-read `frames_written` and discard what the writer overwrote (the newest `kRecordingFrames` frames are the only ones kept).
16. **`ks-engine --backend paced`** (with `--rate`, `--channels`, `--period`, as for the others) replaces the miniaudio device with `PacedBackend`, rendering in real time on its own thread. With `--record-fd` it also records into the segment as requirement 15 says. It's for tests and for `ks-harness`-style listening checks, and a normal `kool_spin` doesn't use it.
17. **`OpenRecording` and `RecordingOpened`** (client to supervisor, and back; types 27 and 28; empty bodies; `RecordingOpened` carries the segment as its one descriptor, a client maps it read-only; request IDs non-zero as for `OpenRing`). The supervisor answers `RecordingOpened` when it has a recording segment, and `NotSupported` when it hasn't. `ks::client` isn't changed: tests use the `ks::ipc` mappers.

### Everything else

18. **Real-time rules:** nothing here runs on the audio thread except the recording write: a `memcpy` of a period's frames into the mapped ring and one release store, in a `noexcept KS_NONBLOCKING` function. It copies only into the engine's own mapping, with the segment's size taken from `fstat` (ADR 0003 §8).
19. **No new dependencies.** ADR 0003's §11 and the segments' section gain a line each (the recording segment, `Attach`'s slot, `--track`, `--clean`); no other ADR changes.
20. **Fuzzing:** the socket-protocol fuzz target decodes the new and changed types (`Attach` with a slot, types 27 and 28) without changes, and runs 1,000,000 inputs clean under the `fuzz` preset.

## Acceptance criteria

All process tests use the paced backend with recording, a 10-minute generated track (a ramp or a sine whose value tells the frame number), and a runtime directory of their own. On Linux, `kill -9` is `SIGKILL` to the engine's process ID (from the lock file). Every test that starts `kool_spin` is blocked in the build-loop sandbox: it runs outside it (`CLAUDE.md`).

- [ ] `[auto]` **Library:** `Attach` with a slot, `Attached` with it, and types 27 and 28 round-trip through the codec, and the decoder refuses a slot of `kStateClients` or more. (test: `tests/acceptance/engine-restart/messages_test.cpp`, "Restart messages: round trip and refusals")
- [ ] `[auto]` **Recording layout:** the header and entries have the sizes and offsets of requirement 15; zero-filled memory is valid; a writer's period appears in the reader's copy after `frames_written`, and a reader that falls behind by more than `kRecordingFrames` is told so. (test: `recording_test.cpp`, "Recording segment: layout, wrap, and slow readers")
- [ ] `[auto]` **Paced engine records:** `ks-engine --backend paced --record-fd` records a sine track at 44.1 kHz, started and paused, and the recording shows silence, then the sine, with no frame missing (its zero crossings are one period apart within 1 frame). (test: `engine_process_test.cpp`, "Engine process: paced backend records")
- [ ] `[auto]` **`--track`:** an engine given a valid track descriptor and a resume state that names it resumes the deck on it; a descriptor of a segment with a different ID is reported and skipped, leaving the deck empty; a bad descriptor, a repeated ID, an ID of 0, and a track that no deck holds (which is released with `TrackReleased`) behave as requirements 5 and 7 say. (test: `engine_process_test.cpp`, "Engine process: --track")
- [ ] `[auto]` **Restart after `kill -9`:** a track plays on deck 0 at rate 1. `kill -9` the engine. Within 5 s the heartbeat grows again, the process ID is new, and the supervisor's report says `restarted in <n> ms`. The recording continues, the new boundary's frame is where the old engine stopped within one period, and the new engine's first frames are the track at the position it would have reached (`kill time` to `restart host time` at rate 1) within 10 ms, rising over 50 ms (the first frame is silent, and the gain is monotonic). The deck's `DeckState` says playing, with the same track ID. The measured restart time is printed; it is under 1 s in `debug` and `release`, and under 3 s under `asan` and `tsan`. (test: `restart_test.cpp`, "Restart: kill -9 resumes at the right position")
- [ ] `[auto]` **Everything comes back:** two tracks on decks 0 and 1, the crossfader at 0.5, a cue point on deck 0, two clients with rings, and deck 1 paused. After `kill -9`, both decks hold the same tracks; the paused deck is still paused at its position; the mixer and the cue point are as before; each client's ring is at the same slot, a command it writes after the restart is applied (its `ClientAck` advances), and a command it wrote while the engine was away is applied once. (test: `restart_test.cpp`, "Restart: decks, mixer, cue point, and rings come back")
- [ ] `[auto]` **Killed mid-load and mid-decode:** the engine is killed (a) while a worker is still decoding the track on deck 0, which then finishes, and its `DecodeEnded` reaches the client; (b) between the supervisor's `LoadTrack` and the engine's answer, using the fake worker's delay, so the client gets `Unavailable` for its `LoadFile`, the track is released, and no worker or descriptor is left behind (`/proc/<pid>/fd` of the supervisor, as in the track-loading tests). (test: `restart_test.cpp`, "Restart: killed while decoding" and "Restart: killed with a load in flight")
- [ ] `[auto]` **Requests during a restart:** with a new engine held in start-up (the paced engine started with a delay option the test probe provides: `--start-delay <ms>`, honoured only by `ks-engine --backend paced`), a `LoadFile`, an `OpenRing`, and an `UnloadDeck` are answered `Unavailable` with the right text; a `Ping` is answered; a client connecting during the restart gets its `Welcome`. (test: `restart_test.cpp`, "Restart: requests during a restart")
- [ ] `[auto]` **Stop during a restart:** `Stop` during start-up of the new engine stops it and the supervisor, with status 0 within 3 s, and no engine process is left. (test: `restart_test.cpp`, "Restart: stop during a restart")
- [ ] `[auto]` **The crash-loop guard:** with an engine path that is the test probe (an engine that exits at once, after `Ready`, with `abort()`), the supervisor restarts it twice, makes a clean restart the third time (the probe is told by `--clean`, and stays up), and with the probe set to keep failing, gives up on the fourth with status 3 and the message of requirement 11, with the clients' connections closed. A real engine killed three times within 30 s comes back the third time with empty, paused decks, and the same ring slots. Exits more than 30 s apart (the window is shortened by `--crash-window <ms>`, a `kool_spin` option the tests use) restart as the first again. (test: `restart_test.cpp`, "Restart: the crash-loop guard")
- [ ] `[auto]` **Start failures count:** an engine that doesn't reach `Ready` in time (the delay option longer than `kEngineReadyTimeout`, shortened by `--ready-timeout <ms>`) is killed, counts as an exit, and is restarted as requirement 11 says. (test: `restart_test.cpp`, "Restart: a start that fails counts")
- [ ] `[auto]` **Corrupted state:** a `ks-engine` run directly on a state segment the test owns plays a track and is killed; the test then writes garbage into both resume slots (a flipped CRC, then an out-of-range rate, then random bytes in the whole segment) and starts a second `ks-engine` on it. For each, the engine starts, reports that it started clean, its decks are paused and empty, its heartbeat grows, and it exits 0 on `SIGTERM`. (test: `engine_process_test.cpp`, "Engine process: corrupted state starts clean")
- [ ] `[auto]` **Log continues:** the log file has the old engine's last lines, then `ks-engine killed by signal 9`, then `restarting ks-engine (restart 1)`, then the new engine's first lines, with timestamps that don't decrease. (test: `restart_test.cpp`, "Restart: the log goes on")
- [ ] `[auto]` **Exit criteria that wait on the recording** (`docs/roadmap.md`):
  - killing a `ks-play` process (a client with a ring that writes commands) with `kill -9` during playback leaves no gap in the recording;
  - killing a decode worker with `kill -9` during playback does the same;
  - deleting a playing track's file does the same (the existing "Supervisor tracks: deleting the file while it plays" test also checks the recording).

  "No gap" means: a 440 Hz sine's zero crossings stay `44100 / 880` frames apart, within 1 frame, over the whole recording, and the recording grows during the test. (test: `restart_test.cpp`, "Chaos: killing a client, a worker, or a file leaves no gap")
- [ ] `[auto]` **Nothing leaks:** after the tests above, no process of the run (engine, worker, client) is left, and the supervisor's descriptors are back to their count before the first restart, apart from the new engine's socket. (test: `restart_test.cpp`, "Restart: descriptors and processes don't leak")
- [ ] `[auto]` **Real-time clean:** the paced backend's recording write is `KS_NONBLOCKING` and passes under `rtsan`; the engine-library tests still pass. (test: `recording_test.cpp`, under `rtsan`)
- [ ] `[auto]` **Fuzzed:** the socket-protocol fuzz target runs 1,000,000 inputs clean under the `fuzz` preset. (test: `ks_socket_protocol_fuzz`)
- [ ] `[auto]` **The earlier steps' tests** all pass. The tests that expected the supervisor to end with status 3 when the engine ended unexpectedly (`supervisor_test.cpp`, "Supervisor: the engine ending") change to expect a restart; that is a protected change the maintainer approves with this spec.
- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux in every preset, and on macOS arm64 in `debug`, `release`, `asan`, and `tsan`.
- [ ] `[manual]` **By hand:** on the Inpulse 500, the maintainer plays two tracks through `ks-play`, `kill -9`s the engine, and hears the music come back within about a second, at the right place, with a fade-in and no click, and the deck positions line up again.

## Out of scope

- **Step 7b:** the heartbeat watch, the pings, killing a hung engine, and the control-thread case that waits for no playing deck.
- **A crash handler that records a backtrace** (ADR 0002): it's a process-wide feature, and it gets its own small step.
- **Restarting workers.** A hung or lost decode worker (the decode-worker spec's decision 4) isn't part of this step.
- **Step 8:** a supervisor that finds an engine from an earlier session.
- **Step 9:** the session journal. A restarted *supervisor* starts clean.
- **`ks-play` commands** for the recording or for restarts. Tests use the `ks::ipc` mappers.
- **UI and controller-script restart behaviour.**
- **Timing guarantees under load:** 1 s is a target, measured and printed, not a hard real-time bound; the hard thresholds above are only to catch regressions.

## Open questions

None. The maintainer approved these proposals on 2026-11-19:

1. **`Attach` gains a slot** (requirement 6). It changes an existing message, and the supervisor picks slots from now on. The alternative: the engine picks the lowest free slot as today, and the supervisor re-attaches rings in slot order, and a client's `ClientAck` slot may move after a restart when an earlier slot was free. I propose the first: clients map their ack by slot, so it mustn't move.
2. **`kRestartTracks` = 16** (requirement 4) is a guess at the most tracks the engine can hold at once. I'll count from the code before writing the tests.
3. **Crash-loop counting** (requirement 11): ADR 0003 says "after three restarts within 30 seconds, the last restart discards". I've read it as: the *third* exit in the window causes the clean restart, and the *fourth* gives up. If you read it as three restarts normal and the fourth clean, change both numbers.
4. **Test-only options** (`--start-delay`, `--crash-window`, `--ready-timeout`): I need them to test timing without sleeping for 30 s. They'd be documented in `--help` as for tests. The alternative is environment variables.
5. **Recording through `OpenRecording`** (types 27 and 28) rather than a file in the runtime directory. On Linux the segment is a memfd, with no path to open, so a message is the only way a separate test process gets it.
6. **The recording ring's size:** 95 s at 44.1 kHz is 32 MiB of address space, committed lazily. Enough for the tests above (longest about 20 s); say if you'd rather have more.
