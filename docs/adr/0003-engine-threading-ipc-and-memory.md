# ADR 0003: Engine threading, IPC, and memory

- **Status:** Proposed
- **Date:** 2026-11-09

## Context

[ADR 0002](0002-reliability-and-process-model.md) splits kool_spin into a supervisor, an engine, a UI, and workers, and says what crosses each boundary: commands go in through lock-free rings, state comes out through seqlocks, decoded tracks live in shared memory, and Unix sockets carry everything that isn't real-time. This ADR decides how:

- the threads inside the engine;
- the layout and rules of the shared memory, and who creates and cleans it up;
- how tracks are held in memory;
- how the engine restarts.

It also settles the roadmap's open design questions and splits Phase 1 in two.

Constraints:

- The real-time rules in `CLAUDE.md`, and ADR 0002's: the engine never waits on another process, and everything it reads from shared memory is untrusted.
- Linux and macOS differ here. macOS has no `memfd_create`, limits POSIX shared-memory names to 31 characters, sizes a shared-memory object only once, and doesn't support `SOCK_SEQPACKET` on Unix sockets.
- miniaudio 0.11's data callback carries no timestamps, and miniaudio has no API for a device's output latency.
- AI agents write most of the code. When two designs are otherwise equal, we take the one that the compiler, the sanitizers, the fuzzers, and the tests can check.

## Decision

### 1. One render function, several drivers

The engine's real-time work is one function, `render(Block&) noexcept KS_NONBLOCKING`. A block carries the frame count, the output buffers, the block's host time, and status flags. Four drivers call it:

| Driver | Used for | Thread |
|---|---|---|
| miniaudio | Playing through a device | The audio backend's thread |
| Offline, fast | Golden and property tests | The test's thread |
| Offline, paced | Chaos and soak tests: runs in real time and records its output; can simulate device loss | Its own |
| Freewheel | Device loss: keeps the clock and the decks running on a timer, and discards the output | Its own |

The freewheel driver is the paced offline driver without the recording, so device loss runs code the tests already exercise.

Only one driver renders at a time. Each driver takes an atomic render token at the start of every block and releases it at the end. A driver that can't take the token skips the block; a device driver outputs silence instead. A late callback from a failing device therefore can't race the freewheel thread.

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
- **Memory model.** A seqlock over plain memory is a data race, which is undefined behavior in C++ and which TSan reports. Payloads are therefore arrays of `std::atomic<std::uint64_t>` accessed with relaxed ordering, and fences order them:
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
- **The supervisor is single-threaded:** one `poll` loop over its sockets, with a self-pipe for `SIGCHLD`.
- **Loading a track:**
  1. A client asks the supervisor.
  2. The supervisor creates a track segment and passes it to a worker and to the engine's control thread at the same time.
  3. The worker decodes into the segment.
  4. Meanwhile, the control thread maps the segment, locks it, and builds a track object. It hands the object to the audio thread through an internal ring (section 10).
  5. Playback can start before decoding finishes (section 9).

### 7. Shared-memory segments

| Segment | One per | Written by | The engine maps it |
|---|---|---|---|
| Engine state | Engine | The engine | Read-write |
| Command ring | Client | The client (slots, head) and the engine (tail) | Read-write |
| Track | Loaded track | One worker | Read-only |
| Log | Process | That process | Not at all |

- **No names.** No process opens a segment by name; segments travel only as file descriptors.
  - **Linux:** `memfd_create` with sealing allowed. Before passing a segment on, the supervisor seals it against shrinking and growing (`F_SEAL_SHRINK` and `F_SEAL_GROW`, then `F_SEAL_SEAL`). The engine checks the seals before mapping. If a segment shrank under the engine's mapping, the engine would crash with `SIGBUS`; the seals rule that out.
  - **macOS:** `shm_open` with `O_CREAT | O_EXCL`, mode 0600, and a random name of about 16 characters, then `ftruncate`, then `shm_unlink` straight away. macOS sizes a shared-memory object only once, which gives the same protection as the seals.
  - **The macOS leak window:** macOS can't list shared-memory names, so if the supervisor died between `shm_open` and `shm_unlink`, the object would leak until reboot. To prevent that, the supervisor records each name in an intent file before creating it, and clears it after unlinking. The next launch unlinks any names left in the file.
- **Lifetime:** the kernel keeps a segment until the last descriptor and the last mapping are gone, in every process. Nothing needs cleaning up after a crash.
- **Layout:**
  - Each segment starts with a header: magic number, layout version, kind, and sizes.
  - Shared structs are standard-layout and trivially copyable, and use fixed-width types. They hold offsets, never pointers. `static_assert` checks their sizes and offsets.
  - Every shared atomic is `is_always_lock_free`.
  - Fields written by different processes are on separate cache lines.
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
- **Total budget:** half of physical memory by default, configurable. A load that would exceed it, judged by the decoder's duration estimate, is refused with a clear error.
- **Progressive playback:** as it decodes, the worker publishes `frames_ready` with release ordering. The engine reads it with acquire ordering. Past that point, the deck plays silence and reports that it's waiting.
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
- **Starting again:** the supervisor starts a new engine and passes it every segment again: the state segment, the clients' rings, and each track with its ID. The engine picks the newest valid slot. A deck whose track ID isn't among the tracks passed comes back empty.
- **Rings:** the engine reads each ring's tail back from shared memory and checks that `head − tail` is within the capacity. A ring that fails the check is reset. Commands are absolute, so a command applied twice does no harm.
- **Position:** decks that were playing resume where they would be if they had kept playing. Each position advances by the time since the snapshot, times the deck's rate, like a turntable that kept spinning, and the same as during device loss (section 15). Beatmatched decks stay aligned. Playback fades in over about 50 ms.
- **Hangs:**
  - The engine-status heartbeat counts rendered blocks. Every driver advances it, freewheel included.
  - The engine's control thread treats a device that stops calling back without notice as device loss.
  - If the heartbeat stops for 500 ms, the supervisor kills the engine with `SIGKILL` and restarts it.
  - If the control thread stops answering the supervisor's pings, the supervisor reports it, but doesn't kill the engine while a deck is playing: that would interrupt working audio. It restarts the engine once no deck is playing.
- **Crash loops** are handled as ADR 0002 says. By default, after three restarts within 30 seconds, the last restart discards the resume state and starts with decks paused. If that also fails, the supervisor stops restarting the engine and reports it.
- **Restart time:** the target is under 1 second, mostly the time to open the device.

### 12. Supervisor crash

- **The other processes keep running.** Loaded decks keep playing. Clients that already have rings keep control through them: play, pause, seek, and the mixer. Loading tracks, restarts, and the session journal stop.
- **Detecting survivors:** each child holds a lock (`flock`) on its own lock file in the runtime directory, and writes its process ID into the file. The kernel releases the lock when the process dies, so a held lock means a live process.
- **Relaunching:** a supervisor that finds a live engine doesn't start a second one. It tells the user that the previous session is still playing, and offers to stop it. If the user agrees, the engine fades out and exits.
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

- **Content:** the supervisor builds the journal from the engine's state blocks and its own record of what's loaded; it knows the file paths because it handled the loads. The journal holds:
  - for each loaded track: its path, size, and modification time (content fingerprints arrive in Phase 2), position, rate, and cue point;
  - the mixer settings.
- **Writing:** at least once a second, atomically: write a temporary file, `fsync` it, `rename` it, then `fsync` the directory.
- **Format:** plain text: a version line, one `key value` line per field with paths escaped, and a final checksum line. It needs no library, is readable when debugging, and is fuzzed.
- **Location:** `$XDG_STATE_HOME/kool_spin/` on Linux (by default `~/.local/state/kool_spin/`), and `~/Library/Application Support/kool_spin/` on macOS.
- **Restoring:** at launch, the session is restored with decks paused. If a track's file is missing, its deck stays empty, with a warning.

### 15. Audio devices

- **Headphone cue:** in Phase 1, the main and cue buses go to channels of one multi-channel device. That covers controllers with built-in audio, including the Inpulse 500 (outputs 1–2 and 3–4). Cue on a second device needs clock-drift compensation, and comes later.
- **Fallback device** (ADR 0002 §4): after Phase 1.
- **Device loss:**
  1. The control thread starts the freewheel driver when miniaudio reports that the device stopped, or when callbacks stop without a report.
  2. It polls the device list for the device's return, because miniaudio doesn't report arrivals.
  3. When the device is back, it reopens it.
  4. The freewheel driver stops once device callbacks hold the render token, and the output fades back in.
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
- **A seqlock over plain memory:** a data race under the C++ memory model, and TSan reports it.
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
