# Feature: Loading tracks

- **Status:** Approved by the maintainer on 2026-11-17. Protected: build loops can't change it.
- **Roadmap:** Phase 1b, step 5b
- **Built by:** interactive

## Goal

Let a client load an audio file onto a deck. The client gives the supervisor a path and a deck. The supervisor opens the file, starts a `ks-worker` for it, checks the probed length against the memory budget, and creates a track segment, which it passes to the worker to decode into and to the engine to play. The engine maps the segment read-only and puts the track on the deck. The deck can play as soon as the first frames are decoded. When no deck holds the track any more, the engine unmaps it and the supervisor frees it. A worker that dies partway leaves the track ending where it got to. The engine never waits on any of this.

## Context

- **ADRs that apply:**
  - **ADR 0002:** workers are disposable: a crash fails that job and nothing else. The engine never reads files, and never waits on another process.
  - **ADR 0003 §6:** the supervisor is the hub. It opens the file, starts a worker, and creates the track segment, then passes the segment to the worker and the engine at the same time. Real-time data never passes through the supervisor. The supervisor is single-threaded.
  - **ADR 0003 §7 and §8:** the engine maps a track segment read-only. It copies shared values, validates them, and takes sizes from its own mapping.
  - **ADR 0003 §9:** each track is capped at 2 GiB of samples, within a total memory budget (by default half of physical memory) that loads are judged against by the probe's estimate. Playback can start before decoding finishes. When no deck holds a track, the engine unmaps it and tells the supervisor, which closes its descriptor.
  - **ADR 0003 §10:** objects reach the audio thread through the give ring, and leave through the retire ring, which the control thread drains.
- **What exists:**
  - **The decode worker** (`specs/decode-worker/spec.md`): `ks-worker --supervisor-fd <n> [--log-fd <n>]` takes one `Probe` (with the open file) and answers `Probed` or `Error` `Refused`, then one `Decode` (with the track segment) and answers `Decoded`. A stop signal makes it finish the track as `Failed` and exit with status 0 without replying. Its socket closing before a `Decode` makes it exit with status 0. Its socket closing during decoding doesn't stop it.
  - **The track segment** (`<ks/ipc/track_segment.h>`): `TrackHeader` (`sample_rate`, `status`, `ready`), `read_track()`, `TrackWriter`, `kTrackSegmentSize`, `kTrackSamplesOffset`, `kMaxTrackFrames` (2²⁸), and `SegmentKind::Track`.
  - **`ks::engine`:** `Track` over private memory, with `publish(frames, complete)` and `ready()`. `Engine::load(deck, track)` gives the engine one reference, which comes back through `collect()` as a `Released` once no deck holds the track. The deck plays only frames published as ready, waits `kInterpolatorReach` (128) frames short of the end of a track that isn't complete, and stops at the end of one that is. The deck's state block reports the track's ID, rate, capacity, frames ready, and whether it's complete.
  - **`kool_spin` and `ks-engine`** (`specs/processes/spec.md`, `specs/client-rings/spec.md`, `specs/logs/spec.md`): clients say `Hello`, and can open a command ring. The supervisor drains the engine's log ring into `ks-engine.log`.
- **This spec extends** `specs/processes/spec.md` requirements 10, 13, 15, 16, 19, 20, and 21; `specs/client-rings/spec.md` requirements 5, 9, and 12; and `specs/logs/spec.md` requirements 10, 15, 18, and 19. Everything else in them still applies.
- **The API exists before the code:** the acceptance tests compile against the public headers. Until the maintainer approves this spec and the tests, the function bodies are stubs, and every acceptance test fails.

## Requirements

### Messages

1. **Five new types,** following `specs/socket-protocol/spec.md` requirements 20 and 21:

   | Type | Number | From, to | Body | Descriptors | Request ID |
   |---|---|---|---|---|---|
   | `LoadFile` | 19 | Client to supervisor | `deck`: u32; `path`: text | 0 | Non-zero |
   | `FileLoaded` | 20 | Supervisor to client | `track_id`: u64; `sample_rate`: u32; `estimated_frames`: u64 | 0 | The `LoadFile`'s |
   | `LoadTrack` | 21 | Supervisor to engine | `track_id`: u64; `deck`: u32; `sample_rate`: u32 | 1: the track segment | Non-zero |
   | `TrackLoaded` | 22 | Engine to supervisor | Empty | 0 | The `LoadTrack`'s |
   | `TrackReleased` | 23 | Engine to supervisor | `track_id`: u64 | 0 | 0 |

   - `deck` is a deck's index, from 0, below `kStateDecks` (4).
   - `path` is an absolute path, 1 to `kMaxPathSize` (4,096) bytes, starting with `/`.
   - `track_id` is never 0. The supervisor chooses it (requirement 11).
   - `FileLoaded` carries what the worker probed: the file's rate, and its estimated length in frames, or 0 if there's no estimate.

   `decode()` refuses, with `BadField`: a `deck` of 4 or more; a `path` that's empty, longer than 4,096 bytes, or doesn't start with `/`; a `track_id` of 0; a `sample_rate` outside `kMinTrackRate` to `kMaxTrackRate`; and an `estimated_frames` above `kMaxTrackFrames`. As for every type, `TrackReleased` with a non-zero request ID, and the other four with request ID 0, are refused with `WrongRequestId`.
2. **Rules from the earlier steps** (`specs/segments/spec.md` requirements 2 to 4) apply: every descriptor is close-on-exec, and system calls interrupted by a signal are retried. Nothing new runs on the audio thread: it already plays `Track`s.

### The engine

3. **From the supervisor,** the engine also takes `LoadTrack`.
4. **`LoadTrack`:** the control thread:
   1. maps the segment read-only, as kind `Track` at `kTrackLayoutVersion`;
   2. takes the capacity from its own mapping: (the mapping's size − `kTrackSamplesOffset`) / 8 frames, at most `kMaxTrackFrames`;
   3. builds a `Track` over the sample area, with that capacity, the message's rate, and its track ID. Nothing is ready yet;
   4. calls `Engine::load()` with the deck and the track;
   5. answers `TrackLoaded`.

   The deck has the track from the next block. If the segment is refused, its capacity is 0, the engine already holds a track with that ID, or `load()` returns false, the engine unmaps the segment, closes its descriptor, and answers `Error` `Unavailable`, saying why. It keeps playing.

   While it holds a track, the engine keeps both its mapping and its descriptor of the segment.
5. **Passing progress on:** the audio thread never reads a track segment's header. On each pass of its loop (at least every 10 ms), the control thread reads the header of each track that isn't finished with `read_track()`, and:
   1. **checks it.** The header is corrupt if `read_track()` refuses it; if its frames are fewer than the last count the control thread passed on; or if its frames are above 0, or it's complete, and its rate isn't the rate `LoadTrack` gave;
   2. **faults in the new frames:** it reads one byte of every page that holds a frame published since the last pass, through its own mapping, so the pages are mapped in the engine before the audio thread reads them. It faults in at most `kFaultInPerPass` (64 MiB) of track memory per pass, across all tracks, and passes on only the frames it has faulted in; the rest wait for the next pass;
   3. **passes the count on** with `Track::publish()`: the frames faulted in, and complete once the header is complete and every frame has been faulted in.

   A corrupt header finishes the track: the control thread publishes the last count it passed on, complete, and reads that header no more. A complete track is finished too. Either way, the track stays on its deck.
6. **Releasing:** each track the engine holds has one reference on the audio thread. When `collect()` returns it, the control thread destroys the `Track`, unmaps the segment, closes its descriptor, and sends `TrackReleased` with the track's ID.
7. **Without a supervisor:** tracks keep playing, their progress is still passed on, and a released track is still unmapped, without a message (ADR 0003 §12).
8. **Its lines,** in the engine's log, with decks numbered from 1, as in `specs/audio-events/spec.md` requirement 7:
   - **info:** a track loaded, with its ID and deck; a track finished as `Complete`, with its frames; a track released, with its ID;
   - **warning:** a `LoadTrack` refused, and why; a track finished as `Truncated` or `Failed`, with its frames; a corrupt header, why, and the frame the track now ends at.

### The supervisor

9. **Command line:** `kool_spin` also takes:
   - `--worker <path>`: the `ks-worker` executable. The default is `ks-worker` in the directory that holds `kool_spin`'s own executable, as for `--engine`;
   - `--track-memory <MiB>`: the track memory budget, a whole number of MiB from 1 to 1,048,576. The default is half of physical memory, from `sysconf(_SC_PHYS_PAGES)` × `sysconf(_SC_PAGESIZE)`, rounded down to a whole MiB.

   A value out of range, or not a number, and a repeated option, are usage errors.
10. **Starting:** after it checks the `engine` lock (`specs/processes/spec.md` requirement 15, step 3), the supervisor checks that the worker is an executable file (`access(X_OK)`), and works out the budget. If either fails, that's a starting failure. It logs the budget, in MiB.
11. **`LoadFile`:** from a client, after `Hello`, the supervisor also takes `LoadFile`. It answers at once:
    - while stopping: with `Error` `NotSupported`, as it answers `Hello`;
    - when `kMaxWorkers` (4) workers are running: with `Error` `Unavailable`;
    - when it can't open the path for reading (with `O_NONBLOCK`, so opening a FIFO can't block it), or it isn't a regular file: with `Error` `Refused`, saying why.

    Otherwise it gives the load the next track ID (from 1, increasing through the session, never reused), creates a log segment for a worker, and starts `ks-worker` with one end of a `socket_pair()` as descriptor 3 and the log segment as descriptor 4: `--supervisor-fd 3 --log-fd 4`. It sends the worker `Probe` with the file, and closes its own descriptor of the file. If the segment, the socket pair, or the worker can't be created or started, it answers `Error` `Unavailable`. The track ID becomes the deck's latest load (requirement 13).

    The supervisor serves its other connections while the worker probes. If the client leaves before it's answered, the load carries on.
12. **`Probed`:** the supervisor checks, in order:
    1. **that the load is still the deck's latest** (requirement 13). If it isn't, it answers `Error` `Refused`, saying that a later load for the deck replaced it;
    2. **the budget** (requirement 16). If the track doesn't fit, it answers `Error` `Refused`, giving the memory the track needs, and how much of the budget is in use, in MiB;
    3. **that it can create the track segment**, with `SegmentFactory::create(SegmentKind::Track, kTrackLayoutVersion, kTrackSegmentSize)`, and map it read-write. If not, it answers `Error` `Unavailable`.

    If any check fails, it closes the worker's connection, so the worker exits. Otherwise it sends the engine `LoadTrack`, with the track ID, the deck, the probed rate, and the segment; and the worker `Decode`, with `max_frames` of `kMaxTrackFrames` and the segment. When the engine answers:
    - **`TrackLoaded`:** the supervisor answers the client `FileLoaded`, with the track ID, the probed rate, and the probed estimate;
    - **`Error`:** it answers the client `Error` `Unavailable`, with the engine's text, and sends the worker `SIGTERM`.
13. **The latest load wins:** for each deck, the supervisor remembers the track ID of the last `LoadFile` it started a worker for. A load whose probe finishes after a later load for the same deck has started is refused (requirement 12). Loads sent to the engine reach the deck in the order they were sent, so the deck ends up with the latest load that succeeds.
14. **From a worker,** the supervisor takes `Probed` (once, after its `Probe`), `Decoded` (once, after its `Decode`), and an `Error`, and answers anything else as `specs/processes/spec.md` requirement 7 says:
    - **`Error` `Refused` before `Probed`:** it answers the client `Error` `Refused`, with the worker's text;
    - **`Error` `Refused` after `Decode`:** it reports it. The worker exits, and requirement 15 finishes the track;
    - **`Decoded`:** decoding has ended, with those frames and that status.

    A frame the connection refuses, or one that doesn't decode, is answered as requirement 7 says, closing the worker's connection, and the supervisor sends the worker `SIGTERM`. When a worker's connection closes, whatever the reason, the rest waits for the worker to end (requirement 15). The supervisor reads each worker's log ring at least every `kLogDrainInterval` (100 ms), as it reads the engine's.
15. **A worker ending:** on `SIGCHLD`, the supervisor reaps its workers as well as the engine. For each worker that has ended, it reads the worker's log ring once more, then:
    - **if the client hasn't been answered and the probe didn't finish:** it answers `Error` `Refused`, saying how the worker ended before it probed the file;
    - **if it was sent `Decode`, and the track's header isn't complete:** decoding stopped partway. The supervisor finishes the track itself, through its read-write mapping: it stores the probed rate, then finishes it as `Failed` (`TrackWriter::finish()`), with the frames `read_track()` reads, or 0 if `read_track()` refuses the header. Decoding has then ended with those frames;
    - **if the engine has released the track, or never took it:** the supervisor closes and unmaps the segment.

    It closes the worker's connection and log segment.
16. **The budget:** a track counts against it from when its segment is created until the supervisor closes the segment. Until decoding ends, it counts its probed estimate × 8 bytes, or `kMaxTrackFrames` × 8 bytes (2 GiB) when the estimate is 0. Once decoding ends, it counts its frames × 8 bytes. A load fits if the total, with it, is at most the budget.
17. **`TrackReleased`:** from the engine, the supervisor also takes `TrackReleased`, and `TrackLoaded` and an `Error` that answer a `LoadTrack` it sent. On `TrackReleased`, if the track's worker is still running, it sends the worker `SIGTERM`, and requirement 15 closes the segment once the worker has ended; otherwise it closes and unmaps the segment at once. A `TrackReleased` for a track it doesn't hold, and a `TrackLoaded` that answers no `LoadTrack`, are answered with `Error` `NotSupported`.
18. **Stopping:** when the supervisor starts stopping, it answers every `LoadFile` not yet answered with `Error` `NotSupported`, and sends every running worker `SIGTERM`. It waits for the workers, as well as the engine, until the stop deadline (`kEngineStopTimeout`), then kills any worker left with `SIGKILL`. It replies `Done` only once the engine and every worker have ended. Whatever the supervisor exits for, no worker it started outlives it: on any other way out, it sends each running worker `SIGTERM`, waits up to `kWorkerStopTimeout` (1 s), and kills the rest with `SIGKILL`. It reaps every worker and reads its log ring once more before it exits. Workers don't change the exit status.
19. **Workers' lines:** each worker's lines go into one file in the session folder, `ks-worker.log`, which the supervisor creates when it starts the first worker, with the header for `ks-worker`. Each line is the worker's record, with `track <id>: ` before its text. A ring's dropped lines, and a corrupt ring, are reported there as for the engine's (`specs/logs/spec.md` requirement 14), with the same prefix; a corrupt ring is also reported in `kool_spin.log`. Without the file, the lines go to standard error, starting `ks-worker: `.
20. **The supervisor's lines,** in `kool_spin.log`, with decks numbered from 1:
    - **info:** the budget, at start; a client asked to load a file, with its name, the path, and the deck; a worker started, with its process ID and the track ID; a track loaded, with its ID and deck; decoding ended `Complete`, with the frames; a track released;
    - **warning:** a load refused, and why; decoding ended `Truncated` or `Failed`, with the frames; a worker's `Error`, with its text;
    - **error:** a worker ended without finishing its track, so the supervisor finished it (requirement 15), with how the worker ended and the frame the track now ends at; a worker's log ring found corrupt.

### Lines tests look for

21. **Fixed phrases,** added to `specs/logs/spec.md` requirement 19's. `<id>` is a track ID, `<n>` a deck number from 1, `<pid>` a process ID, and `<s>` a signal number:

    | File | Level | Contains |
    |---|---|---|
    | `kool_spin.log` | `info` | `started ks-worker (process <pid>) for track <id>` |
    | Same | `info` | `track <id> loaded on deck <n>` |
    | Same | `info` | `track <id> released` |
    | Same | `warning` | `refused client '<name>' a load` |
    | Same | `error` | `track <id>: ks-worker was killed by signal <s>` |
    | `ks-worker.log` | as the worker's | `track <id>: ` before each of the worker's phrases (`specs/decode-worker/spec.md` requirement 20) |
    | `ks-engine.log` | `info` | `loaded track <id> on deck <n>` |
    | Same | `info` | `released track <id>` |
    | Same | `warning` | `track <id>'s header is corrupt` |

### The ADR

22. **ADR 0003 §9** changes in two places:
    - **Total budget** gains: "A file with no estimate counts as the per-track cap. Once a track has decoded, its actual size counts instead."
    - **Progressive playback** becomes: "As it decodes, the worker publishes `frames_ready` with release ordering. The engine's control thread reads it with acquire ordering, checks it, faults in the newly decoded pages, and locks them when it can, before it passes the count on to the audio thread. The audio thread therefore never reads the shared header, and never takes the first page fault on track memory. Past that point, the deck plays silence and reports that it's waiting."

## Acceptance criteria

The tests are in `tests/acceptance/track-loading/`. They follow `specs/processes/spec.md`'s rules for tests: separate runtime directories, the null backend, and no process outliving its test. Tests that start `kool_spin` pass `--log-dir` inside their own temporary directory, as `specs/logs/spec.md` asks.

**The fake worker:** a helper program built for these tests, `ks_fake_worker`, takes `ks-worker`'s command line and answers the same messages, but its behavior comes from the file it's asked to probe: a text file starting `ks-fake-track`, whose lines set the rate and estimate it reports; a delay before it answers `Probe`, or a refusal, or no answer at all; how many frames it decodes, in what chunks, and how fast; and how it ends: `Complete`, `Failed`, killed by `SIGKILL` without finishing the track, or hanging. It can also block `SIGTERM`, so that only `SIGKILL` stops it. Its frames are zero. Tests that use it start `kool_spin` with `--worker` pointing at it.

"A deck shows" below means: within 5 s, that deck's state block shows it.

Messages (`messages_test.cpp`):

- [ ] `[auto]` **Round trip:** each new type encodes and decodes to the same message, and encodes again to the same bytes: `LoadFile` with deck 0 and a one-byte path `/`, and with deck 3 and a 4,096-byte path with multi-byte UTF-8; `FileLoaded` with an estimate of 0 and of `kMaxTrackFrames`; `LoadTrack` with rates 8,000 and 384,000; `TrackLoaded`; and `TrackReleased` with track ID 2⁶⁴ − 1. (test: "Track messages: round trip")
- [ ] `[auto]` **Refused:** each of these is refused with the right error:
  - `LoadFile` with deck 4, an empty path, a 4,097-byte path, and the path `music/a.flac`;
  - track ID 0 in `FileLoaded`, `LoadTrack`, and `TrackReleased`;
  - rates 7,999 and 384,001 in `FileLoaded` and `LoadTrack`; an estimate of `kMaxTrackFrames` + 1 in `FileLoaded`; deck 4 in `LoadTrack`;
  - `LoadTrack` with no descriptor, and with 2; `LoadFile`, `FileLoaded`, `TrackLoaded`, and `TrackReleased` with 1;
  - request ID 0 for `LoadFile`, `FileLoaded`, `LoadTrack`, and `TrackLoaded`; request ID 1 for `TrackReleased`;
  - a body one byte short, and one byte long, for each type.

  (test: "Track messages: what's refused")
- [ ] `[auto]` **Fuzzed:** the socket-protocol fuzz target decodes the new types without further changes, and still runs 1,000,000 inputs clean under the `fuzz` preset. (test: `ks_socket_protocol_fuzz`)

The engine process (`engine_tracks_test.cpp`). The test plays the supervisor, as in `specs/client-rings/spec.md`'s engine tests, gives the engine a log segment, and plays the worker too: it creates track segments as requirement 12 says and writes them with a `TrackWriter`:

- [ ] `[auto]` **Load, play, and release:** with 44,100 frames already published, a `LoadTrack` for deck 0 with track ID 7 at 44,100 Hz is answered with `TrackLoaded`. Deck 0 shows track 7, rate 44,100, and 44,100 frames ready, not complete. The test finishes the track as `Complete` at 88,200 frames; deck 0 shows 88,200 frames ready, complete. Through a ring, `SetPlaying` makes deck 0's position advance. A second `LoadTrack`, for deck 0 with track ID 8, is answered with `TrackLoaded`, and then a `TrackReleased` for track 7 arrives, with request ID 0. On Linux, the engine's `/proc/<pid>/maps` then no longer maps track 7's segment (found by its device and inode). The engine's log has `loaded track 7 on deck 1` and `released track 7`. (test: "Engine tracks: load, play, and release")
- [ ] `[auto]` **Playing while decoding:** a track loaded with nothing published shows 0 frames ready. Playing it, with 4,410 frames then published, deck 0 shows `kDeckWaiting`, with its position at 4,410 − 128 frames. When the test publishes 44,100 frames and finishes the track as `Complete`, deck 0 shows it complete, and its position stops at 44,100 with `kDeckWaiting` clear. (test: "Engine tracks: playing while decoding")
- [ ] `[auto]` **Pages are faulted in:** on Linux, with a track loaded and paused, the test publishes 441,000 frames (3,528,000 bytes). Within 1 s, the `Rss` that `/proc/<pid>/smaps` gives the engine's mapping of that segment is at least 3,445 kB. (test: "Engine tracks: published pages are faulted in")
- [ ] `[auto]` **What it refuses:** each of these is answered with `Error` `Unavailable`, and the engine's heartbeat keeps increasing:
  - a `LoadTrack` with an engine-state segment;
  - one with a track segment whose layout version is 2;
  - one with a track segment of `kTrackSamplesOffset` bytes, which holds no frames;
  - one with the ID of a track the engine holds.

  Afterwards a valid `LoadTrack` is answered with `TrackLoaded`. (test: "Engine tracks: what it refuses")
- [ ] `[auto]` **A corrupt header:** three tracks are loaded, on decks 0, 1, and 2, at 44,100 Hz. On the first, the test publishes 1,000 frames, waits for deck 0 to show them, and then stores status 7. On the second, it publishes 2,000 frames, waits for deck 1 to show them, and then stores 1,000. On the third, it starts at 48,000 Hz and publishes 3,000 frames. Each deck then shows its track complete, with 1,000, 2,000, and 0 frames ready. Publishing 10,000 frames on each afterwards changes none of them within 500 ms. The heartbeat keeps increasing, and the engine's log has a warning containing `header is corrupt` for each of the three tracks. (test: "Engine tracks: a corrupt header")
- [ ] `[auto]` **Without a supervisor:** with a track loaded and 1,000 frames published, the test closes its end of the socket, then publishes 44,100 frames and finishes the track as `Complete`. Deck 0 shows 44,100 frames ready, complete. `SIGTERM` ends the engine with exit status 0. (test: "Engine tracks: progress without the supervisor")

The supervisor (`supervisor_tracks_test.cpp`). The test spawns `kool_spin` with `ks-engine`, and with `ks-worker` unless it says it uses the fake worker. The files come from `tests/acceptance/decode-worker/data/`, or the test writes them:

- [ ] `[auto]` **Loading a file:** after `Hello`, a `LoadFile` of the 1-second FLAC on deck 1 is answered with `FileLoaded`, with a non-zero track ID, 44,100 Hz, and an estimate of 44,100 frames. Deck 1 shows that track, with 44,100 frames ready, complete. Through a ring, `SetPlaying` makes deck 1's position advance. `kool_spin.log` has `started ks-worker (process <pid>) for track <id>` and `track <id> loaded on deck 2`; `ks-worker.log` is well-formed and has a line with `track <id>: ` and `decoded `; and `ks-engine.log` has `loaded track <id> on deck 2`. (test: "Supervisor tracks: loading a file")
- [ ] `[auto]` **A client that leaves:** a client sends a `LoadFile` of the FLAC for deck 0 and closes its connection at once. A second client sees deck 0 show a track, 44,100 frames ready, complete. (test: "Supervisor tracks: a load outlives its client")
- [ ] `[auto]` **Replacing a track releases it:** after the FLAC is loaded on deck 0 as track A, the MP3 is loaded on deck 0 as track B. Deck 0 shows B, and within 2 s `kool_spin.log` has `track <A> released` and `ks-engine.log` has `released track <A>`. (test: "Supervisor tracks: replacing a track releases it")
- [ ] `[auto]` **Files it refuses:** each of these is answered with `Error` `Refused`, and the client stays connected: a path that doesn't exist; a directory; a FIFO, after which a `Ping` is answered within 1 s; and a text file, whose answer has the same text as the decoder's refusal of it in the test's own process. A `LoadFile` with a relative path is answered with `Error` `BadMessage`, and the connection is closed. `kool_spin.log` has `refused client '<name>' a load` for each refusal. (test: "Supervisor tracks: files it refuses")
- [ ] `[auto]` **The budget:** with the fake worker and `--track-memory 1` (1,048,576 bytes). Each track decodes as many frames as its estimate unless this says otherwise, and each step waits for the one before:
  - a track with an estimate of 100,000 frames (800,000 bytes) loads on deck 0;
  - a second with an estimate of 100,000 frames, on deck 1, is answered with `Error` `Refused`, and the text contains `MiB`;
  - one with no estimate, on deck 1, is refused too;
  - a track with an estimate of 20,000 frames (160,000 bytes) loads on deck 0, replacing the first. Once `kool_spin.log` reports the first released, a track with an estimate of 100,000 frames loads on deck 1: 960,000 bytes are in use;
  - a track with an estimate of 10,000 frames (80,000 bytes) that decodes only 1,000 loads on deck 2. Once deck 2 shows it complete, it counts 8,000 bytes instead of 80,000, so a track with an estimate of 10,000 frames loads on deck 3, which it wouldn't fit otherwise.

  (test: "Supervisor tracks: the memory budget")
- [ ] `[auto]` **Four workers at most:** with the fake worker, four `LoadFile`s of tracks that never answer `Probe` get no answer within 500 ms, and a fifth is answered with `Error` `Unavailable`. One of the four blocks `SIGTERM`. A `Stop` is then answered with `Done`, and `kool_spin` exits with status 0 within 3 s. Each of the four `LoadFile`s was answered with `Error` `NotSupported` first. Every worker process ID that `kool_spin.log` reported is gone within 1 s of `kool_spin`'s exit. (test: "Supervisor tracks: four workers at most")
- [ ] `[auto]` **The latest load wins:** with the fake worker, track A, which answers `Probe` after 500 ms, is loaded on deck 0, then track B, which answers at once. B's `LoadFile` is answered with `FileLoaded`, then A's with `Error` `Refused`. Deck 0 shows B, and still does 1 s later. (test: "Supervisor tracks: the latest load wins")
- [ ] `[auto]` **A worker dying while it decodes:** with the fake worker, a track at 44,100 Hz publishes 20,480 frames in chunks of 4,096, 50 ms apart, then kills itself with `SIGKILL`. Deck 0 plays from as soon as `FileLoaded` arrives. Deck 0 then shows 20,480 frames ready, complete, and its position stops at 20,480. The heartbeat keeps increasing, and `kool_spin.log` has `track <id>: ks-worker was killed by signal 9`. (test: "Supervisor tracks: a worker dying while it decodes")
- [ ] `[auto]` **Deleting the file while it plays:** a 10-minute WAV that the test writes is loaded on deck 0. As soon as `FileLoaded` arrives, the test deletes the file and starts deck 0 playing. Deck 0 shows 26,460,000 frames ready, complete, within 30 s, and its position keeps advancing. (test: "Supervisor tracks: deleting the file while it plays")
- [ ] `[auto]` **Usage:** `--track-memory 0`, `--track-memory 1048577`, `--track-memory x`, and a repeated `--track-memory` each make `kool_spin` exit with status 2 and a line starting `kool_spin: `. A `--worker` that doesn't exist makes it exit with status 1 and a line starting `kool_spin: `. (test: "Supervisor tracks: usage")

Everywhere:

- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux in every preset, and on macOS arm64 in `debug`, `release`, `asan`, and `tsan`.

## Out of scope

- **Step 6:** `ks-play` and a client library; a message to unload a deck; telling clients how decoding ended. Clients see progress in the deck's state block, and the status in the logs.
- **Step 7:**
  - time limits for a worker that hangs while it probes or decodes. Until then, a hung worker holds one of the four places until `kool_spin` stops;
  - decoding again after a worker crash, rather than ending the track;
  - giving a restarted engine its tracks again;
  - the recording segment, with which the chaos tests check that killing a worker leaves no gap in the output.
- **Step 8:** a new supervisor adopting the tracks of a running engine.
- **Step 10:** locking track memory, and the health checks.
- **The session journal,** which will record each loaded file's path.
- **Paths that aren't valid UTF-8:** a `LoadFile` path is text (requirement 1), so a file whose name isn't valid UTF-8 can't be loaded.
- **Opening a file never blocks for long, assumed:** `open()` on a hung network file system or failing drive would hold up the supervisor's loop. The engine plays on regardless.
- **Sharing:** loading the same file twice decodes it twice, into two segments.

## Open questions

None. The maintainer decided (2026-11-17):

1. **The control thread passes progress on** (requirement 5): the audio thread reads only the private `Track`, never the shared header, and never takes the first page fault on a page of track memory. The cost is up to one control interval (10 ms) before newly decoded frames reach the deck. Step 10's locking goes in the same place.
2. **The budget counts the estimate while decoding** (requirement 16), as ADR 0003 §9 says, and the actual size after. A track is never cut short because its estimate was low; the budget can be exceeded while a track with a bad estimate decodes.

These follow from them, from the ADRs, and from the decode-worker spec:

3. **The supervisor finishes a track whose worker died** (requirement 15), in the header, as the decode-worker spec's decision 4 promised. The header stays the one record of how far a track decoded, so a restarted engine (step 7) reads the same thing.
4. **`FileLoaded` comes once the engine has the track** (requirement 12), not once it's decoded: the deck can play straight away, and the state block shows decoding's progress.
5. **The latest load for a deck wins** (requirement 13), whatever order the probes finish in.
6. **Hung workers wait for step 7** (Out of scope), with the rest of the restart and time-limit logic, as the decode-worker spec left restarting a decode to step 7.
7. **One `ks-worker.log` per session** (requirement 19), with each line naming its track, rather than a file per worker.
8. **Faulting in is limited per pass** (requirement 5): faulting in a whole 2 GiB track at once would hold the control thread for a quarter of a second or more, delaying device-loss handling.
9. **The fake worker** (Acceptance criteria) makes slow, failing, crashing, and hanging workers repeatable, without the test having to find and signal a real worker at the right moment.
