# ADR 0003: Engine threading, IPC, and memory

- **Status:** Accepted
- **Date:** 2026-11-09

## Context

[ADR 0002](0002-reliability-and-process-model.md) splits kool_spin into a supervisor, an engine, a UI, and workers, and says what crosses each boundary: commands go in through lock-free rings, state comes out through seqlocks, decoded tracks live in shared memory, and Unix sockets carry everything that isn't real-time. This ADR decides how:

- the threads inside the engine;
- the layout and rules of the shared memory, and who creates and cleans it up;
- how tracks are held in memory;
- how the engine restarts.

It also settles the roadmap's open design questions and splits Phase 1 in two. It amends ADR 0001's inter-process communication entry (section 7) and ADR 0002's supervisor-crash behavior (section 12).

Constraints:

- The real-time rules in `CLAUDE.md`, and ADR 0002's: the engine never waits on another process, and everything it reads from shared memory is untrusted.
- Linux and macOS differ here. macOS has no `memfd_create`, limits POSIX shared-memory names to 31 characters, sizes a shared-memory object only once, and doesn't support `SOCK_SEQPACKET` on Unix sockets.
- miniaudio 0.11's data callback carries no timestamps, and miniaudio has no API for a device's output latency.
- AI agents write most of the code. When two designs are otherwise equal, we take the one that the compiler, the sanitizers, the fuzzers, and the tests can check.

## Decision

### 1. One render function, several drivers

The engine's real-time work is one function, `render(Block&) noexcept KS_NONBLOCKING`. A block carries the frame count, the sample rate, the output buffers, the block's host time, and status flags. Four drivers call it:

| Driver | Used for | Thread |
|---|---|---|
| miniaudio | Playing through a device | The audio backend's thread |
| Offline, fast | Golden and property tests | The test's thread |
| Offline, paced | Chaos and soak tests: runs in real time and records its output; can simulate device loss | Its own |
| Freewheel | Device loss: keeps the clock and the decks running on a timer, and discards the output | Its own |

The freewheel driver is the paced offline driver without the recording, so device loss runs code the tests already exercise.

Only the active driver renders, and only one driver renders at a time:

- **The active driver.** The control thread chooses which driver is active. A block from any other driver is skipped.
- **The render token.** Each driver also takes an atomic render token at the start of every block and releases it at the end, so two drivers never render at once, even while the control thread is switching between them. A driver that can't take the token skips the block.
- **Skipped blocks are silent.**
- **What this guarantees.** A late callback from a failing device can't race the freewheel thread, and handing over between drivers never renders the same stretch of time twice.
- **Fading in.** The first block after a handover, or after the engine starts, is flagged, so the output safety stage fades in.

### 2. Engine threads

- **Audio:** whichever driver holds the render token. There is one audio thread, and decks are not rendered in parallel: four interpolating decks use a small fraction of one core. Revisit with key lock.
- **Control:** one thread, not real-time. It:
  - owns the socket to the supervisor;
  - maps and unmaps segments, and locks track memory;
  - builds objects for the audio thread, and frees the ones it retires (section 10);
  - opens, closes, and reattaches the audio device;
  - runs the health checks.
- miniaudio's notification callbacks only set flags that the control thread acts on.

Phase 4 adds MIDI and controller-script threads. They send to the audio thread through the same ring type as clients do (section 4), in private memory.

### 3. Blocks, timing, and determinism

- **Callback size:** miniaudio runs with `noFixedSizedCallback = true`, so it calls us with whatever the backend delivers instead of buffering to a fixed size. The engine splits large callbacks into sub-blocks of at most `kMaxBlockFrames`, so its scratch buffers are allocated once.
- **Command timing:** live commands take effect at the start of the next block. The offline drivers split blocks so that scheduled commands take effect on their exact frame.
- **Block-size invariance:** given the same commands at the same frames, the output doesn't depend on how the frames were divided into blocks. Smoothing, gain ramps, and the limiter therefore work per sample and carry their state across blocks. Property tests render one script with several block sizes (for example 1, 37, 128, and 4096) and require bit-identical output within one build.
- **Golden files** are compared with a tolerance, not bit for bit. Bit-exact results across platforms aren't realistic: Clang fuses multiply-adds on arm64 but not on baseline x86-64, and the math libraries differ.
- **Sample rate:** the engine runs at the device's rate. Decks keep their position in track frames, so a device that comes back at a different rate changes only the step per output frame.
- **Visual play position:** miniaudio gives no timestamps, so the engine works it out:
  - it reads the system's monotonic clock at the start of each callback;
  - it smooths those readings with a delay-locked loop, as JACK does;
  - it adds the buffer latency and an estimate of the device's own latency.

  Phase 3 measures the error on hardware. If it's too large, native CoreAudio or JACK backends go behind `AudioBackend`.

### 4. Commands in

- **One ring per client.** Each client gets its own single-producer, single-consumer ring, in its own segment (section 7).
- **Fixed-size slots.** Each slot is 64 bytes, one cache line: command type, target (such as a deck or bus), the client's sequence number, and a fixed-size payload. There is no variable-length framing, so decoding is a pure function from a slot's bytes to a validated command or an error. That makes it easy to fuzz.
- **Counters.** Head and tail are 64-bit counters that never wrap in practice, on separate cache lines. The capacity is a power of two.
- **Absolute values.** Commands carry absolute values: "rate 1.02", "seek to frame N", "playing". Replaying one after an engine restart is harmless.
- **Acknowledgements.** The engine publishes the last sequence number it applied for each client (section 5). Clients learn what has been applied without a socket round trip.
- **Fairness.** Each block, the engine takes at most a fixed number of commands from each ring, visiting the rings in a fixed order. When several clients control the same thing, the last command applied wins.
- **Loads go elsewhere.** Loading a track doesn't go through a ring. Mapping a segment is a system call, so loads go through the control thread (section 6).

### 5. State out

- **Blocks.** The engine publishes its state through seqlocks:
  - one block per deck;
  - one for the mixer;
  - one for engine status: device state, underrun and NaN counts, callback load, heartbeat, and the last sequence number applied for each client;
  - the resume state (section 11).
- **Writers and readers.** The audio thread is the only writer. No real-time thread ever reads a seqlock.
- **Memory model.** A seqlock over plain memory is a data race, which is undefined behavior in C++ and which TSan reports. Payloads are therefore arrays of `std::uint64_t`, read and written only through `std::atomic_ref` with relaxed ordering (section 7), and fences order them:
  - **writer:** store `seq + 1` (relaxed); release fence; store the payload; store `seq + 2` (release);
  - **reader:** load `seq` (acquire), retrying while it's odd; load the payload into a local copy; acquire fence; load `seq` again (relaxed), and retry if it changed.
- **What checks it.** TSan doesn't understand standalone fences, so it can't catch an ordering mistake here. The stress tests on macOS arm64 are the check: arm64's weaker memory ordering exposes bugs that x86 hides.
- **Visual play position.** The engine publishes a track frame and the host time at which that frame reaches the output (section 3).

### 6. Topology and sockets

- **The supervisor is the hub.** Every socket connects to the supervisor. The engine has exactly one socket, to the supervisor, and trusts no other peer.
- **Connecting.**
  - Child processes inherit one end of a `socketpair()` when they're started, so they need no socket files.
  - External clients (`ks-play`, test harnesses) connect to a socket in a per-user directory, created with mode 0700: `$XDG_RUNTIME_DIR/kool_spin/` on Linux and `$TMPDIR/kool_spin/` on macOS.
  - The supervisor checks that a client has the same user ID (`SO_PEERCRED` on Linux, `getpeereid` on macOS).
- **The supervisor creates every segment** and passes it as a file descriptor (`SCM_RIGHTS`) to each process that needs it. It keeps its own descriptor until the segment is no longer needed. Segments therefore outlive any child, and the supervisor can give the engine everything again after a restart (section 11).
- **Real-time data never passes through the supervisor.** Clients write rings, the engine writes state blocks, and workers write tracks.
- **Wire format:** `SOCK_STREAM`, with length-prefixed binary messages (version, type, length, body) up to a fixed maximum size. The parser is hand-written and fuzzed.
  - `SOCK_SEQPACKET` would keep message boundaries, but macOS doesn't support it on Unix sockets.
  - A serialization library would add a dependency to the supervisor.
- **The supervisor is single-threaded:** one `poll` loop over its sockets, with a self-pipe for `SIGCHLD`. Its one other thread writes the session journal (section 14), so that a slow or hung disk never holds up the loop. The loop hands it the newest journal, and never waits for it.
- **Loading a track:**
  1. A client asks the supervisor, giving the file's path.
  2. The supervisor opens the file and starts a worker, giving it the open file. The worker reports the file's sample rate and estimated length, or why it can't decode it.
  3. If the track fits the per-track cap and the memory budget (section 9), the supervisor creates a track segment and passes it to the worker and to the engine's control thread at the same time.
  4. The worker decodes into the segment.
  5. Meanwhile, the control thread maps the segment, locks it, and builds a track object. It hands the object to the audio thread through an internal ring (section 10).
  6. Playback can start before decoding finishes (section 9).
  7. When decoding ends, the supervisor tells the client that asked (`DecodeEnded`), after it has answered the load. A client unloads a deck by asking the supervisor (`UnloadDeck`), which asks the engine's control thread.

### 7. Shared-memory segments

| Segment | One per | Written by | The engine maps it |
|---|---|---|---|
| Engine state | Engine | The engine | Read-write |
| Command ring | Client | The client (slots, head) and the engine (tail) | Read-write |
| Track | Loaded track | One worker | Read-only |
| Log | Process | That process | Not at all |
| Recording | Session, with `kool_spin --recording`; tests only | The engine's paced backend | Read-write |

- **No names.** No process opens a segment by name; segments travel only as file descriptors.
  - **Linux:** `memfd_create` with sealing allowed. Before passing a segment on, the supervisor seals it against shrinking and growing (`F_SEAL_SHRINK` and `F_SEAL_GROW`, then `F_SEAL_SEAL`). The engine checks the seals before mapping. If a segment shrank under the engine's mapping, the engine would crash with `SIGBUS`; the seals rule that out.
  - **macOS:** `shm_open` with `O_CREAT | O_EXCL`, mode 0600, and a random name of about 16 characters, then `ftruncate`, then `shm_unlink` straight away. macOS sizes a shared-memory object only once, which gives the same protection as the seals.
  - **The macOS leak window:** macOS can't list shared-memory names, so if the supervisor died between `shm_open` and `shm_unlink`, the object would leak until reboot. To prevent that, the supervisor records each name in an intent file before creating it, and clears it after unlinking. The next launch unlinks any names left in the file.
- **Lifetime:** the kernel keeps a segment until the last descriptor and the last mapping are gone, in every process. Nothing needs cleaning up after a crash.
- **Layout:**
  - Each segment starts with a header: magic number, layout version, kind, and sizes.
  - Shared structs are standard-layout and trivially copyable, and use fixed-width types. They hold offsets, never pointers. `static_assert` checks their sizes and offsets.
  - Words that two processes access at the same time are plain `std::uint64_t`, read and written only through `std::atomic_ref<std::uint64_t>`, which must be always lock-free. Plain integers keep the structs trivially copyable, so zero-filled memory is a valid value and nothing has to be constructed in shared memory.
  - Fields written by different processes are at least 128 bytes apart, so they never share a cache line: Apple Silicon's cache lines are 128 bytes.
  - A process refuses a segment whose magic number, version, or kind doesn't match.

### 8. Reading shared memory

- **Copy, validate, use.** The engine copies a shared value into private memory, validates it, and uses only the copy. It never reads a shared field twice expecting the same value.
- **Sizes are the engine's own.** The engine takes a segment's size from `fstat` when it maps the segment, never from a header another process wrote. A worker's `frames_ready` is clamped to that capacity. Other fields, such as the sample rate and enum values, are range-checked.
- **The engine's state is private.** The engine keeps the authoritative copy of everything it writes to shared memory (ring tails, state) in private memory. It reads the shared copies back only at restart, and validates them like any other input.
- **Samples can be NaN or Inf.** The output safety stage silences them. Phase 1 has no recursive filters. When they arrive (EQ), they must reset when their state stops being finite.
- **Fuzz targets:** the command decoder, the socket message parser, the resume-state validator, the track segment header, and the session journal parser.

### 9. Track memory

- **Format:** interleaved stereo, 32-bit float, at the file's sample rate. The worker mixes other channel layouts to stereo. That's about 21 MB per minute at 44.1 kHz and 46 MB per minute at 96 kHz. The deck's interpolator converts the rate as it plays, so there's one interpolation stage.
- **Per-track cap: 2 GiB of samples**, about 100 minutes at 44.1 kHz or 47 minutes at 96 kHz.
  - Every track segment is sized at the cap. Pages are committed only when the worker writes them, so the unused part costs address space, not memory. Phase 1b verifies this lazy commitment on macOS.
  - The engine never reads past `frames_ready`.
  - A file whose estimated duration exceeds the cap is refused before decoding. If the estimate was wrong and decoding reaches the cap, the worker stops there and flags the track as truncated. What was decoded keeps playing.
- **Total budget:** half of physical memory by default, configurable. A load that would exceed it, judged by the decoder's duration estimate, is refused with a clear error. A file with no estimate counts as the per-track cap. Once a track has decoded, its actual size counts instead.
- **Progressive playback:** as it decodes, the worker publishes `frames_ready` with release ordering. The engine's control thread reads it with acquire ordering, checks it, faults in the newly decoded pages, and locks them when it can, before it passes the count on to the audio thread. The audio thread therefore never reads the shared header, and never takes the first page fault on track memory. Past that point, the deck plays silence and reports that it's waiting.
- **Memory locking:**
  - The engine calls `mlockall(MCL_CURRENT)` after starting and after each device opens.
  - It locks each track in chunks as the track decodes.
  - It never uses `MCL_FUTURE`. With a low `RLIMIT_MEMLOCK` (the kernel default is 8 MiB), every later `mmap` would fail.
  - Locking is best effort. When it fails, the health check warns.
- **Release:** when no deck holds a track, the engine unmaps it and tells the supervisor, which closes its descriptor. The kernel frees the memory when the last mapping is gone.

### 10. The audio thread's memory

- **Allocated before it runs:** everything the audio thread uses:
  - deck slots up to `kMaxDecks` (the shared layout has 8 slots; Phase 1 uses 4);
  - mixer buses;
  - scratch buffers for `kMaxBlockFrames`.
- **Handing objects over:** objects reach the audio thread through an in-process ring of owning pointers, the give ring. They leave through a second ring, the retire ring, which the control thread drains and frees. The retire ring is the deferred-deletion queue from `CLAUDE.md`.
- **Nothing else:** no `shared_ptr`, RCU, or hazard pointers on the audio thread.

### 11. Engine restart

- **Resume state:** every block, the engine publishes what it needs to resume:
  - for each deck: the track ID, position, rate, play state, cue point, and gain;
  - the mixer settings;
  - the host time.

  It alternates between two slots. Each slot is a seqlock with a generation number and a CRC-32.
  - A crash during a write leaves the other slot intact.
  - Memory corruption before a crash fails the CRC or the range checks. The engine then starts clean, with decks paused.
- **Starting again:** the supervisor starts a new engine and passes it the state segment, the log segment, and each track it holds as an inherited descriptor named `--track <id>:<fd>`, so the engine maps the tracks before it builds the resuming `Engine` and opens the device. The clients' rings are attached again after `Ready`, by the `Attach` message, which names the slot the ring must take, so a client's `ClientAck` slot never moves. The engine picks the newest valid slot. A deck whose track ID isn't among the tracks passed comes back empty, and a track passed that no deck holds is released with `TrackReleased`. Requests that waited for the old engine are answered `Unavailable`, and new ones are too until the rings are attached again.
- **Rings:** the engine reads each ring's tail back from shared memory and checks that `head − tail` is within the capacity. A ring that fails the check is reset. The engine reads a command without moving the tail, and moves the tail past it only after a resume slot holds its effect. A command read by an engine that dies before publishing is then read again by the next one, not lost. Commands are absolute, so a command applied twice does no harm.
- **Loads and unloads:** the engine answers `LoadTrack` and `UnloadDeck` only after a resume slot holds the change, so an engine that dies first leaves the request unanswered, never answered and lost. A `TrackReleased` waits behind any answer still waiting, so the supervisor never hears of a track's release before its load.
- **Position:** decks that were playing resume where they would be if they had kept playing. Each position advances by the time since the snapshot, times the deck's rate, like a turntable that kept spinning, and the same as during device loss (section 15). Beatmatched decks stay aligned. Playback fades in over about 50 ms.
- **Hangs:**
  - The engine-status heartbeat counts rendered blocks. Every driver advances it, freewheel included.
  - The engine's control thread treats a device that stops calling back without notice as device loss.
  - If the heartbeat stops for 500 ms, the supervisor kills the engine with `SIGKILL` and restarts it. It polls every 100 ms (a fifth of the timeout, at least 10 ms) and gives an engine whose heartbeat has never been non-zero 5 seconds to start it, so that one that never starts beating is killed too (`specs/hang-detection/spec.md`).
  - If the control thread stops answering the supervisor's pings, the supervisor reports it, but doesn't kill the engine while a deck is playing: that would interrupt working audio. It restarts the engine once no deck is playing. It pings every 1 second, and the control thread is unresponsive when a ping has gone unanswered for 3 intervals. `kool_spin --hang-timeout` and `--ping-interval` change the thresholds, for tests.
- **Crash loops** are handled as ADR 0002 says. The supervisor counts engine exits in a sliding window of 30 seconds, a failure to start or to send `Ready` in time included. The first and second exits restart as above. The third makes a clean restart: the supervisor zero-fills the engine-state segment past its header, passes no tracks, and starts the engine with `--clean`, so decks come back empty and paused. The fourth gives up: the supervisor reports it, closes the clients, and exits with status 3.
- **Recording:** for chaos tests, `kool_spin --recording` creates a recording segment, a ring of the newest 2²² stereo frames, with a count of frames written and a boundary for each engine start. It outlives the engine, and the engine's paced backend (`--backend paced --record-fd`) writes the output of each period into it, from the audio thread, with one release store. Clients get it with `OpenRecording`.
- **Restart time:** the target is under 1 second, mostly the time to open the device.

### 12. Supervisor crash

- **The other processes keep running.** Loaded decks keep playing, and a track still decoding is decoded to its end. Clients that already have rings keep control through them: play, pause, seek, and the mixer; `ks::client::Session` keeps its ring and its mapping of the engine's state when its connection is lost. Loading tracks, restarts, hang detection, and the session journal stop. The engine runs until a signal stops it, and its log lines have no reader.
- **Detecting survivors:** the engine holds a lock (`flock`) on its own lock file in the runtime directory, and writes its process ID into the file. The kernel releases the lock when the process dies, so a held lock means a live process. The engine takes the lock just before it writes its ID, so the ID is believed only when two probes in a row, 100 ms apart, agree. An `acquire()` retries for a moment, so that a probe can't make it fail.
- **Two locks each** (`specs/state-folder/spec.md`): the supervisor and the engine also hold a lock of the same name in the journal's folder (section 14), so that a relaunch finds an engine whose runtime directory was a different one, or has been removed.
  - **The runtime directory's is mandatory:** the socket and the macOS intent file beside it depend on it, and on Linux it's in memory.
  - **The journal folder's is best effort.** It's on disk, and a full or read-only disk must not keep an engine from restarting. If it's held by another process, the launch ends, as with the runtime directory's; anything else that goes wrong with it is a warning.
  - **Finding a survivor** looks in both, the journal's folder first.
- **Relaunching:** a supervisor that finds a live engine doesn't start a second one. It tells the user that an engine from an earlier session is still running, and offers to stop it (`specs/supervisor-crash/spec.md`):
  - **The offer:** it asks, if it's in the foreground of a terminal, and the default is no. `kool_spin --stop-previous` says yes without being asked. Otherwise the answer is no: it exits with status 1, and the engine plays on.
  - **Stopping:** once everything that can be checked beforehand has been, it sends `SIGTERM`, and the engine fades out and exits. If the lock is still held after 2 s, it sends `SIGKILL`. Then it starts a new session. Before each signal it checks that the lock still names the same process, and on Linux it signals through a pidfd, so a reused process ID isn't hit.
- **`ks-play`** says that an engine is running without `kool_spin`, but never stops it.
- **Adoption is deferred:** a new supervisor taking over the running engine would be better, but isn't needed yet. The design leaves room for it: the engine keeps its segments' descriptors, so it could hand them to a new supervisor.

### 13. Process lifecycle and logs

- **Starting:** the supervisor starts children with `posix_spawn`, each in its own process group. Ctrl-C in a terminal then reaches only the supervisor, which stops the others with a fade-out.
- **Surviving the parent:** children don't ask to be killed when their parent dies (no `PR_SET_PDEATHSIG`). They must outlive the supervisor.
- **Logs:** each process logs through a ring in a shared-memory segment that the supervisor created, and the supervisor writes the rings to per-process log files.
  - After a crash, the supervisor still has the dead process's last lines, so the crash handler only needs to record a backtrace.
  - Non-real-time threads write to the ring under a mutex.
  - The audio thread writes fixed-size records (an event code and a few numbers) to a private lock-free ring. The control thread formats them into the log.
  - When a log ring is full, new lines are dropped and counted.
  - Timestamps come from the system's monotonic clock, which all processes share. Each log file records the wall-clock time it started.

### 14. Session journal

- **Content:** the supervisor builds the journal from the engine's state blocks and its own record of what's loaded; it knows the file paths because it handled the loads. The journal holds (`specs/session-journal/spec.md`):
  - for each deck with a track: the file's path, size, and modification time when it was opened (content fingerprints arrive in Phase 2), the track's sample rate, and the deck's position, rate, cue point, and whether it plays;
  - the mixer settings;
  - the machine's boot ID, the engine's process ID, and the monotonic time of the state it records.
- **Writing:** `session.journal` in the state folder, by the supervisor's writer thread (section 6), only while the supervisor holds the folder's lock (section 12). Every second (`--journal-interval`), it reads the state blocks, and if anything but the time has changed, it writes the journal atomically: a temporary file, synced (`F_FULLFSYNC` on macOS), renamed over the journal, and the directory synced. So a paused session isn't rewritten, and a playing one is never more than a second behind. Nothing is written before the engine's first block, so a launch that fails leaves the journal as it was. A write that fails is a warning, and never stops the music. When the supervisor stops, it writes once more, and waits up to 2 s.
- **Format:** plain text: a version line, then `key value` lines in a fixed order, and a final CRC-32 line. Reals are written in their shortest exact form, and paths with `\` doubled and control bytes as `\xHH`, so each journal has exactly one text. It needs no library, is readable when debugging, and is fuzzed: whatever the parser accepts formats back to the same bytes.
- **Location:** `$XDG_STATE_HOME/kool_spin/` on Linux (by default `~/.local/state/kool_spin/`), and `~/Library/Application Support/kool_spin/` on macOS. Without `HOME`, the home directory is the one the user database gives. `--state-dir` names another folder; `--runtime-dir` without it uses none.
- **Not shared with another machine:** the folder is never used on a network file system; on FUSE, which may hide one, only if `--state-dir` names it. A lock there may be another machine's, and so may the process ID in its file. Nor is it used if the group or others can write it.
- **Restoring:** at launch, before the engine starts (after stopping an orphan, if there is one), unless `--new-session` is given. Only writing needs the folder's lock; restoring needs only a journal that can be read. The supervisor opens each entry's file and probes it in a worker, as for a load, waiting at most 3 s for the probes; it then writes a resume slot (generation 1, every deck paused, the journal's mixer) and starts the engine with the restored tracks as `--track` (section 11), so the engine resumes from it as after a restart. Decoding goes on after the engine has started. Restoring never fails a launch:
  - **A journal that's refused** (not a regular file, unreadable, or not one the parser accepts) is a warning; the session starts empty, and a refused journal is kept as `session.journal.rejected`.
  - **An entry that can't be restored** (a missing file, one the worker refuses, a probe too slow, a track over the budget) leaves its deck empty, with a warning, and is *carried*: kept in the journal, not playing, until a client loads or unloads that deck, so that a later launch tries again (a USB drive that wasn't mounted may be by then).
  - **A changed file** (its size, modification time, or sample rate differs) is restored at position 0 and cue point 0, with a warning: positions in the old file may not mean anything in the new one.
  - **After an orphan,** whose decks kept playing while the journal stood still: if the journal's boot ID is this machine's and its engine is the orphan, each deck that was playing moves on by its rate times the time from the journal's state until the orphan was sent `SIGTERM`, by the monotonic clock. Otherwise nothing moves; either way, every deck is restored paused.
  - **A first engine that fails** with a restored session (it exits, or isn't ready in time) is replaced once by one started clean, with the restored entries carried.

### 15. Audio devices

- **Headphone cue:** in Phase 1, the main and cue buses go to channels of one multi-channel device. That covers controllers with built-in audio, including the Inpulse 500 (outputs 1–2 and 3–4). Cue on a second device needs clock-drift compensation, and comes later.
- **Fallback device** (ADR 0002 §4): after Phase 1.
- **Device loss:**
  1. The control thread starts the freewheel driver when miniaudio reports that the device stopped, or when callbacks stop for 100 ms without a report. The freewheel driver starts where the device's last block ended and catches up, so the clock doesn't lose the time it took to notice.
  2. It polls the device list for the device's return, because miniaudio doesn't report arrivals.
  3. When the device is back, it reopens it.
  4. Once the device is calling back, the control thread makes it the active driver and stops the freewheel driver, and the output fades back in.
- **Real-time priority:** no rtkit, which would bring D-Bus into the engine.
  - Device threads get whatever priority their backend gives them. PipeWire's JACK layer manages its own.
  - The freewheel thread asks for `SCHED_FIFO` on Linux, which needs `RLIMIT_RTPRIO`, and for a time-constraint policy on macOS.
  - The health check reports when a thread doesn't get the priority it asked for.

### 16. Phase 1 is split in two

- **Phase 1a, engine library:** the engine as a library, in one process.
  - The shared-memory primitives are built with the layouts above, but backed by anonymous memory. The engine sees only their interfaces.
  - Also in 1a: the drivers and device loss; the deck, mixer, and output safety stage; resume-state validation; and a development harness for listening and soak tests.
- **Phase 1b, processes:** the supervisor, segments, sockets, decode worker, `ks-play`, restart, hang detection, the session journal, and the chaos tests.
- **Why:** the real-time core is the riskiest code, and it gets the fastest feedback. When a chaos test fails in 1b, the cause is in process code, not DSP. Because this ADR fixes the layouts, 1b changes what backs the primitives, not the engine's interfaces.

The [roadmap](../roadmap.md) lists each part's contents and exit criteria.

## Consequences

- **The supervisor grows.** It contains a message router and parser, but stays single-threaded and dependency-free, and its parser is fuzzed.
- **The visual play position depends on our estimates,** because miniaudio gives no timestamps. If Phase 3 finds the estimates too inaccurate, we write native CoreAudio and JACK backends.
- **Long tracks can't be loaded:** anything over about 100 minutes at 44.1 kHz. Each loaded track reserves 2 GiB of address space, but uses memory only for what has been decoded.
- **After a supervisor crash, nothing new can be loaded** until the user stops the orphaned engine, which stops the music.
- **Tolerance hides small changes.** Golden files compared with a tolerance don't catch changes below it. The block-size property tests are bit-exact, but only within one build.
- **macOS can leak a segment.** On macOS, a supervisor crash between creating and unlinking a segment leaks it until the next launch.
- **Commands cost more to add.** Every new command type touches the decoder and its fuzz corpus, and possibly the layout version.

## Alternatives considered

- **An engine-owned render thread feeding the device through a FIFO:** device switches would be seamless, but it adds a buffer of latency, and a second clock that drifts from the device's.
- **Rendering decks in parallel:** unnecessary at Phase 1's load. Revisit with key lock.
- **The render token alone, without an active driver:** while the freewheel thread and a returning device both call back, they render in turn, so the clock runs ahead by up to a block, and a late callback from a dying device can still render.
- **A seqlock over plain memory:** a data race under the C++ memory model, and TSan reports it.
- **`std::atomic` in shared structs:** not trivially copyable, so zero-filled memory isn't a valid value, and every atomic would have to be constructed in shared memory before any process used it.
- **Triple buffering for state:** supports only one reader, and state blocks have several.
- **Variable-length ring messages:** more framing to validate and fuzz. Bulk data doesn't belong in rings anyway.
- **Sockets between every pair of processes, or a client socket in the engine:** more parsing of untrusted input in the engine, and what's running would be known to two processes instead of one.
- **Opening segments by name, or files in `/dev/shm`:** stale names after crashes, and races over names.
- **Mach memory entries on macOS:** no names at all, but more platform-specific code. Worth revisiting if the macOS leak window matters.
- **`SOCK_SEQPACKET`:** Linux only.
- **A serialization library** (Protocol Buffers, Cap'n Proto, FlatBuffers): a dependency in the supervisor.
- **`shared_ptr`, RCU, or hazard pointers to hand objects to the audio thread:** harder to review, and harder for `KS_NONBLOCKING` to check, than two rings.
- **`mlockall(MCL_FUTURE)`:** later mappings fail once the lock limit is reached.
- **Decoding long tracks in windows:** playback would depend on the drive again (ADR 0002 §3). If long mixes matter later, the way forward is to hold them compressed in memory and decode ahead on a non-real-time thread.
- **Storing 16-bit samples:** halves memory, but loses precision on 24-bit sources.
- **Resampling to the device rate in the worker:** a second interpolation stage, and tracks would have to be decoded again whenever the device rate changes.
- **Resuming at the crash position:** the decks would jump back by the downtime. Extrapolating matches device loss and keeps beatmatched decks together.
- **Adopting a running engine after a supervisor crash:** deferred (section 12).
- **Bit-exact golden files across platforms:** needs `-ffp-contract=off` everywhere and identical math libraries, which is fragile.
- **rtkit for real-time priority on Linux:** brings D-Bus into the engine.
