# Feature: ks-play and the client library

- **Status:** Approved by the maintainer on 2026-11-18. Protected: build loops can't change it.
- **Roadmap:** Phase 1b, step 6
- **Built by:** interactive for the three new messages and their handling in the supervisor and engine (requirements 1 to 11); a build loop for the client library and `ks-play` (requirements 12 to 33), once the maintainer has approved this spec and its acceptance tests.

## Goal

Give people and tests a way to drive a running `kool_spin`: a client library, `ks::client`, and `ks-play`, a command-line program built on it. One `ks-play` run connects, does one thing, and exits: load a file onto a deck, unload a deck, send one control command, show the state, or stop `kool_spin`. Two gaps that step 5 left for this step are closed with it: a message that unloads a deck, and a message that tells a client how decoding ended.

## Context

- **ADRs that apply:**
  - **ADR 0003 §4:** a client writes commands into its own ring, with absolute values, and reads what the engine applied from its slot in the status block.
  - **ADR 0003 §5:** state comes out through seqlocks in the engine-state segment, which every client maps read-only. A reader copies a block, retries when the seqlock says it changed, and validates what it copied.
  - **ADR 0003 §6:** the supervisor is the hub, and an external client's only socket is the supervisor's. The supervisor checks the user ID at both ends.
  - **ADR 0003 §7:** shared memory is written by other processes: a client copies and validates what it reads, and takes each segment's size from its own mapping.
- **What exists:**
  - **`ks::ipc`:** `connect_to_supervisor()`, `Connection`, the messages up to `TrackReleased` (type 23), `map_segment`-style mappers, `CommandRing` with `RingProducer`, `CommandSlot`, `EngineState` with its seqlocks, and `TrackStatus`.
  - **`kool_spin`** (`specs/processes/spec.md`, `specs/client-rings/spec.md`, `specs/track-loading/spec.md`): a client says `Hello` and gets the engine-state segment in `Welcome`; `OpenRing` gets a command ring (at most 8 at once); `LoadFile` loads a file onto a deck and is answered with `FileLoaded` once the engine has the track; `Stop` stops everything.
  - **`ks::engine`:** `Engine::unload(deck)` asks the audio thread to unload a deck. The track comes back through `collect()` as a `Released`, and the control thread already sends `TrackReleased` for it (`specs/track-loading/spec.md` requirement 6).
  - **The tests' helpers:** the logs, client-rings, processes, and track-loading tests' support headers, including `ks_fake_worker`, which `kool_spin --worker` can use in place of `ks-worker`.
- **This spec extends** `specs/processes/spec.md` requirements 10, 18, and 21; `specs/track-loading/spec.md` requirements 11, 13, 17, and 18; and `specs/logs/spec.md` requirements 19 and 20 (the new phrases). Everything else in them still applies.
- **The API exists before the code:** the acceptance tests compile against the public headers. Until the maintainer approves this spec and the tests, the function bodies are stubs, and every acceptance test fails.

## Requirements

### Messages

1. **Three new types,** following `specs/socket-protocol/spec.md` requirements 20 and 21:

   | Type | Number | From, to | Body | Descriptors | Request ID |
   |---|---|---|---|---|---|
   | `UnloadDeck` | 24 | Client to supervisor, and supervisor to engine | `deck`: u32 | 0 | Non-zero |
   | `DeckUnloaded` | 25 | Supervisor to client, and engine to supervisor | Empty | 0 | The `UnloadDeck`'s |
   | `DecodeEnded` | 26 | Supervisor to client | `track_id`: u64; `frames`: u64; `status`: u8 | 0 | 0 |

   - `deck` is a deck's index, below `kStateDecks` (4).
   - `track_id` is never 0. `frames` is at most `kMaxTrackFrames`. `status` is a `TrackStatus` other than `Decoding`.

   `decode()` refuses, with `BadField`: a `deck` of 4 or more; a `track_id` of 0; `frames` above `kMaxTrackFrames`; and a `status` that isn't a `TrackStatus`, or is `Decoding`. `UnloadDeck` and `DeckUnloaded` with request ID 0, and `DecodeEnded` with a non-zero one, are refused with `WrongRequestId`.
2. **Rules from the earlier steps** (`specs/segments/spec.md` requirements 2 to 4) apply. Nothing new runs on the audio thread: it already unloads decks.

### The engine

3. **`UnloadDeck`:** from the supervisor, the engine also takes `UnloadDeck`. The control thread calls `Engine::unload(deck)` and, if it returns true, answers `DeckUnloaded`. It answers `Error` `Unavailable`, saying why, if `unload()` returns false (its queue to the audio thread is full). A deck with no track is unloaded all the same, and answered `DeckUnloaded`. The deck is empty from the next block, and the track it held is released as in `specs/track-loading/spec.md` requirement 6.
4. **Its line,** in the engine's log, with decks numbered from 1: **info:** `unloaded deck <n>`.

### The supervisor

5. **`UnloadDeck`:** from a client, after `Hello`, the supervisor also takes `UnloadDeck`. While it is stopping, it answers `Error` `NotSupported`, as for `LoadFile`. Otherwise it sends the engine `UnloadDeck`, with its own request ID, and the client's request waits for the engine's answer:
   - **`DeckUnloaded`:** the supervisor answers the client `DeckUnloaded`;
   - **`Error`:** it answers the client `Error` `Unavailable`, with the engine's text.

   If the client leaves first, the unload carries on. The engine's `DeckUnloaded` and `Error` for an `UnloadDeck` the supervisor didn't send are answered with `Error` `NotSupported`, as for `TrackLoaded`.
6. **An unload cancels loads in progress:** `UnloadDeck` makes the deck's latest load (`specs/track-loading/spec.md` requirement 13) the deck's unload. A `LoadFile` for that deck whose probe finishes later is refused, with `Error` `Refused`, saying that the deck was unloaded.
7. **The track it held** is released as for a replaced track (`specs/track-loading/spec.md` requirement 17): `TrackReleased` from the engine, and then the supervisor closes the segment. A track still decoding when its deck is unloaded has its worker sent `SIGTERM` at that point.
8. **`DecodeEnded`:** the supervisor sends the client that sent a `LoadFile` a `DecodeEnded`, with request ID 0, once decoding has ended (`Decoded` from the worker, or the supervisor finishing the track itself, `specs/track-loading/spec.md` requirement 15), with the frames and status that requirement 14 and 15 record. Rules:
   - it follows the `FileLoaded` for that `LoadFile`, always: if decoding ends first, the supervisor holds the `DecodeEnded` until it has sent `FileLoaded`;
   - a load that was refused, or whose track the engine refused, is answered with `Error` and has no `DecodeEnded`;
   - a client that has left gets nothing, and nothing is kept for it;
   - a track whose worker was sent `SIGTERM` because its deck was unloaded, replaced, or released, or because `kool_spin` is stopping, gets a `DecodeEnded` if its client is still connected, with the status the track ended with (normally `Failed`, or `Truncated`).
9. **Its line,** in `kool_spin.log`, with decks numbered from 1: **info:** `client '<name>' asked to unload deck <n>`.
10. **Fixed phrases,** added to `specs/logs/spec.md` requirement 19's:

    | File | Level | Contains |
    |---|---|---|
    | `kool_spin.log` | `info` | `client '<name>' asked to unload deck <n>` |
    | `ks-engine.log` | `info` | `unloaded deck <n>` |

11. **The ADR:** ADR 0003 §6's list of the steps in loading a track gains a seventh step: `DecodeEnded` after the load is answered, and `UnloadDeck` through the supervisor.

### The client library

12. **`ks::client`** is a library in `src/client/`, with public headers in `src/client/include/ks/client/`. It depends only on `ks::core`, `ks::ipc`, the standard library, and POSIX: it never links Qt, miniaudio, or the decoder, so the UI can use it. It runs on the caller's thread and never on the audio thread. Its failures are returned, never thrown.
13. **`Session::connect(name, runtime_dir)`** (`<ks/client/session.h>`): connects with `connect_to_supervisor()`, with the process's own user ID as `expected_uid`; sends `Hello` with `name`; waits for `Welcome`; and maps the engine-state segment read-only (`SegmentKind::EngineState`, at `kEngineStateLayoutVersion`, taking the size from its own mapping). It gives up with a timeout error after `timeout` (default 5 s). An `Error` from the supervisor, a connection that closes, and a segment it refuses are returned as errors, each of its own kind. The `Session` owns the connection and the mapping.
14. **`Session::open_ring()`** sends `OpenRing`, waits for `RingOpened` (retrying for up to `timeout` while the answer is `Error` `Unavailable`, as `specs/client-rings/spec.md` requirement 4 allows), and maps the ring's segment read-write (`SegmentKind::CommandRing`, `kCommandRingLayoutVersion`, the size from its own mapping). Then it waits, within the same `timeout`, until the engine's status block shows the slot attached: until then the block still shows the slot's earlier occupant, and its `last_sequence` and `rejected` would be taken for this ring's. It returns a `Timeout` error if the slot never shows. It returns the slot. A second call returns an error: a session has at most one ring.
15. **`Session::send(command)`** (`<ks/client/command.h>`) writes one `CommandSlot` into the ring and returns its sequence number. Sequence numbers start at 1 and increase by 1. It returns an error, and writes nothing, with no ring or with the ring full. Each `CommandType` has a function that builds the slot from absolute values: `set_playing(deck, on)`, `set_rate(deck, rate)`, `seek(deck, frame)`, `set_cue_point(deck, frame)`, `set_trim(channel, db)`, `set_fader(channel, position)`, `set_crossfader_assign(channel, side)`, `set_channel_cue(channel, on)`, `set_crossfader(position)`, `set_crossfader_curve(transition)`, `set_main_level(db)`, `set_headphone_level(db)`, `set_cue_mix(position)`, and `set_bus_map(main, cue)`. They check nothing about their arguments beyond their types: the engine decides what is valid (`specs/commands/spec.md` requirement 4), and `Session` reports its answer (requirement 16).
16. **`Session::wait_applied(sequence)`** waits until the engine's status block shows this session's slot has read the command with that sequence number (`last_sequence` has reached it), or until `timeout` (default 2 s), polling at most every 1 ms. Each call reports what the engine did since the previous call (or since `open_ring()`, for the first, when the slot's counts were 0): it compares the slot's `rejected` count with the count the previous call saw, and keeps the new one. It returns:
    - **corrupt**, as soon as the slot is flagged corrupt;
    - **rejected**, with the rejection's name (`unknown type`, `bad target`, `nonzero padding`, `bad value`) from the slot's `last_rejection`, if `rejected` has risen, once the command has been read;
    - **applied**, once the command has been read and `rejected` hasn't risen;
    - **timed out**, otherwise.

    It reads the slot's `ClientAck` through the seqlock and uses only its copy. A caller that sends several commands before it waits gets one answer for all of them: `rejected`, with the last rejection, if the engine rejected any.
17. **`Session::snapshot()`** returns a copy of the whole engine state, read through each block's seqlock, with every field validated: an output state, crossfader side, or rejection code that isn't one of the listed values makes the snapshot an error naming the field, and a real is kept as it is, even if it is NaN or infinite (it's data to show). The copy of one block is consistent; blocks may come from different moments. `Session::snapshot()` never blocks for more than 10 ms in total, and returns an error naming the block if a seqlock stays unstable that long.
18. **`Session::load_file(deck, path)`** sends `LoadFile` and returns the `FileLoaded`'s track ID, rate, and estimate, or the `Error`'s code and text, after `timeout` (default 30 s: the supervisor answers once the probe and the engine are done). **`Session::unload_deck(deck)`** sends `UnloadDeck` and waits for `DeckUnloaded` or `Error`. **`Session::stop()`** sends `Stop` and waits for `Done`, with a timeout of 10 s. **`Session::ping()`** sends `Ping` and waits for `Pong`.
19. **Messages that arrive while waiting:** a request waits for the reply whose request ID matches, with request IDs from 1 increasing by 1. Messages with other request IDs, and notifications, are queued, up to 64: **`Session::next_decode_ended(timeout)`** returns the first queued `DecodeEnded`, or waits for one, returning a timeout error after `timeout`. A message that isn't a reply to something this session asked, or a `DecodeEnded`, is ignored. A frame the connection refuses ends the session: every later call returns the same error.
20. **`Session::close()`**, and destroying the session, closes the connection, which detaches the ring. No call after the connection has closed blocks.

### ks-play

21. **The command line:** `ks-play [--runtime-dir <dir>] [--name <name>] <command> [arguments]`.
    - `--runtime-dir` is the directory that holds `supervisor.sock`. The default is the directory `kool_spin` uses (`specs/socket-protocol/spec.md` requirement 15).
    - `--name` is the `Hello` name, 1 to 64 bytes, default `ks-play`.
    - `--help` prints the usage to standard output and exits with status 0.
22. **The commands.** A deck is a whole number from 0 to 3, and a channel is the same. A real is a decimal number with an optional sign, a fraction, and an exponent, parsed strictly (no leading or trailing characters, no hexadecimal, no `inf` or `nan`); `-inf` is accepted only where the table says. Frames are whole numbers or reals as `CommandType` says.

    | Command | Arguments | Does |
    |---|---|---|
    | `load` | `<deck> <path> [--wait]` | `LoadFile` with the absolute form of `path` (resolved against the working directory, without following symbolic links), then prints `loaded track <id> on deck <deck>: <rate> Hz, estimated <frames> frames`. With `--wait`, it then waits for `DecodeEnded` (requirement 24) |
    | `unload` | `<deck>` | `UnloadDeck`, then prints `unloaded deck <deck>` |
    | `play`, `pause` | `<deck>` | `SetPlaying` 1 or 0 |
    | `rate` | `<deck> <rate>` | `SetRate` |
    | `seek` | `<deck> <frame>` | `Seek` |
    | `cue-point` | `<deck> <frame>` | `SetCuePoint` |
    | `trim` | `<channel> <db>` | `SetTrim`; `-inf` allowed |
    | `fader` | `<channel> <position>` | `SetFader` |
    | `assign` | `<channel> a\|b\|thru` | `SetCrossfaderAssign` |
    | `channel-cue` | `<channel> on\|off` | `SetChannelCue` |
    | `crossfader` | `<position>` | `SetCrossfader` |
    | `curve` | `<transition>` | `SetCrossfaderCurve` |
    | `main-level`, `headphone-level` | `<db>` | `SetMainLevel`, `SetHeadphoneLevel`; `-inf` allowed |
    | `cue-mix` | `<position>` | `SetCueMix` |
    | `bus-map` | `<main> <cue>` | `SetBusMap`, with whole numbers |
    | `status` | `[--watch] [--interval <ms>]` | Shows the engine's state (requirement 25) |
    | `stop` | | `Stop`: `kool_spin` fades out and exits; prints `stopped` |
    | `ping` | | `Ping`; prints `pong` |

    `ks-play` doesn't check a value's range beyond its type: the engine rejects what it doesn't allow, and `ks-play` reports that (requirement 23). `--interval` is a whole number of milliseconds from 50 to 60,000, default 500.
23. **A control command** opens a ring, sends one command, waits for the engine to apply it (requirement 16), and then exits. It prints `ok` when the engine applied it, and exits with status 1 and a line starting `ks-play: ` when it's rejected (naming the rejection), the ring's slot is corrupt, or the wait times out. It prints nothing else to standard output.
24. **`load --wait`:** after the `FileLoaded` line, it waits for `DecodeEnded` (with no time limit of its own: decoding is bounded by the track's size, and `ks-play` can be interrupted) and prints `decoded <frames> frames: complete`, `truncated` or `failed`. It exits with status 0 only for `complete`.
25. **`status`** prints a snapshot (requirement 17) as lines, in this order, and exits. With `--watch`, it prints a snapshot every interval until it gets `SIGINT` or `SIGTERM` (then it exits with status 0) or the connection closes (status 1), with a line `---` before each snapshot after the first. One line for the engine, then one for each deck (0 to 3), each channel (0 to 3), the mixer, and each attached client slot (0 to 7), each of `name=value` fields separated by single spaces, with these names, in this order. Numbers are decimal, reals with `%.6g`, and flags `0` or `1`:
    - `engine`: `heartbeat`, `state` (`stopped`, `playing`, `freewheeling`, or `attaching`), `sample_rate`, `muted`, `silenced`, `late_blocks`, `non_finite_samples`, `limited_frames`, `render_load`, `peak_render_load`;
    - `deck <n>`: `loaded`, `playing`, `waiting`, `complete`, `track`, `track_rate`, `track_frames`, `frames_ready`, `position`, `rate`, `cue`;
    - `channel <n>`: `trim_db`, `fader`, `side` (`a`, `b`, or `thru`), `cue`;
    - `mixer`: `crossfader`, `curve`, `main_db`, `headphone_db`, `cue_mix`, `bus_main`, `bus_cue`;
    - `client <n>`: `corrupt`, `commands_read`, `last_sequence`, `rejected`, `last_rejection` (`none`, `unknown type`, `bad target`, `nonzero padding`, or `bad value`; the value has no spaces, so spaces in the name are written `_`), for slots whose ring is attached, or corrupt.
26. **Status lines to standard output only:** a program that reads `status`'s output never sees anything else there. Everything else (errors, progress) goes to standard error.
27. **Exit statuses:** 0 for success; 1 for failure after connecting (the supervisor or engine refused, a command was rejected, a timeout, the connection closed); 2 for a usage error, with a line starting `ks-play: ` and a pointer to `--help`, before connecting; 3 when it can't reach `kool_spin` (the socket doesn't exist, or nothing is listening, or the listener's user ID isn't the process's own), with a line starting `ks-play: ` saying so.
28. **Errors** are one line to standard error, starting `ks-play: `, and give the reason as `kool_spin` gave it: an `Error` from the supervisor is shown with its code (`refused`, `unavailable`, `not supported`, `bad message`) and text.
29. **Signals:** `ks-play` handles `SIGINT` and `SIGTERM` by closing its connection and exiting with status 130 and 143 (except in `status --watch`, which exits with 0). A `SIGPIPE` doesn't kill it: a closed standard output ends it with status 1.
30. **The library and the executable are split,** as `CLAUDE.md` asks: `src/apps/play/` holds a library `ks_play_app` (argument parsing, the commands, and the output formatting) and a thin `main.cpp`. It builds next to `kool_spin` in `build/<preset>/bin`.

### What isn't protected by this spec's tests

31. **No new dependencies.** `ks::client` and `ks-play` use only what `ks::ipc` and the standard library provide. No ADR changes beyond requirement 11.
32. **Real-time rules:** none of this code is reachable from the audio callback. `ks::client` must still follow the shared-memory rule: copy, validate, use the copy.
33. **Fuzzing:** `ks-play`'s argument parser is fuzzed under `fuzz` (`ks_play_args_fuzz`), with arguments built from the input. It must never crash, and every command line it accepts must be one that requirement 22's table allows.

## Acceptance criteria

The tests are in `tests/acceptance/ks-play/`. They follow `specs/processes/spec.md`'s rules for tests: separate runtime directories, the null backend, and no process outliving its test. Tests that start `kool_spin` pass `--log-dir` inside their own temporary directory. Tests that run `ks-play` run the built executable, with `--runtime-dir`, and check its exit status, standard output, and standard error separately.

Messages (`messages_test.cpp`):

- [ ] `[auto]` **Round trip:** `UnloadDeck` with decks 0 and 3, `DeckUnloaded`, and `DecodeEnded` with each of `Complete`, `Truncated`, and `Failed`, with 0 and `kMaxTrackFrames` frames, and with track ID 2⁶⁴ − 1, each encode and decode to the same message, and encode again to the same bytes. (test: "Play messages: round trip")
- [ ] `[auto]` **Refused:** each of these is refused with the right error: `UnloadDeck` with deck 4; `DecodeEnded` with track ID 0, with `kMaxTrackFrames` + 1 frames, with status `Decoding`, and with status 200; a descriptor on each of the three; request ID 0 for `UnloadDeck` and `DeckUnloaded`; request ID 1 for `DecodeEnded`; a body one byte short, and one byte long, for each. (test: "Play messages: what's refused")
- [ ] `[auto]` **Fuzzed:** the socket-protocol fuzz target decodes the new types without further changes, and still runs 1,000,000 inputs clean under the `fuzz` preset. (test: `ks_socket_protocol_fuzz`)

The engine process (`engine_unload_test.cpp`), with the test as the supervisor, as in `specs/track-loading/spec.md`'s engine tests:

- [ ] `[auto]` **Unloading:** a track loaded on deck 0 is unloaded with `UnloadDeck`, which is answered with `DeckUnloaded`. Deck 0 then shows no track, and a `TrackReleased` for it arrives with request ID 0. An `UnloadDeck` for the now empty deck 0 is answered with `DeckUnloaded` too, and one for deck 1, which never had a track. `ks-engine.log` has `unloaded deck 1`. The heartbeat keeps increasing. (test: "Engine unload: a loaded deck, an empty deck")
- [ ] `[auto]` **Unloading while playing:** with deck 0 playing, an `UnloadDeck` is answered with `DeckUnloaded`; deck 0 then shows no track and isn't playing, the other deck that plays keeps advancing, and the engine's `late_blocks` doesn't rise. (test: "Engine unload: while playing")

The supervisor (`supervisor_unload_test.cpp`), with the test as the client, `ks-engine`, and `ks_fake_worker` or `ks-worker`:

- [ ] `[auto]` **Unloading a deck:** after the FLAC is loaded on deck 2, an `UnloadDeck` for deck 2 is answered with `DeckUnloaded`; deck 2 shows no track; within 2 s `kool_spin.log` has `track <id> released` and `client '<name>' asked to unload deck 3`, and `ks-engine.log` has `unloaded deck 3`. (test: "Supervisor unload: a loaded deck")
- [ ] `[auto]` **An unload cancels a load:** with the fake worker, a track that answers `Probe` after 1 s is loaded on deck 0. At once, `UnloadDeck` for deck 0 is answered with `DeckUnloaded`, and the `LoadFile` is answered with `Error` `Refused`, with a text that contains `unloaded`. Deck 0 shows no track 2 s later. (test: "Supervisor unload: an unload cancels a load")
- [ ] `[auto]` **Unloading a deck that decodes:** with the fake worker, a track of 10 minutes at 44,100 Hz decoding at a tenth of real time is loaded on deck 0. After `FileLoaded`, `UnloadDeck` for deck 0 is answered with `DeckUnloaded`, the worker process ends within 2 s (`kill(pid, 0)` fails with `ESRCH`), and the client receives a `DecodeEnded` for the track with status `Failed` or `Truncated`. (test: "Supervisor unload: a track still decoding")
- [ ] `[auto]` **Who gets `DecodeEnded`:** the FLAC loaded on deck 0 by client A: A gets `FileLoaded` and then `DecodeEnded` with that track ID, 44,100 frames, and `Complete`; a second connected client B gets nothing within 1 s. With the fake worker, a track that decodes 20,480 frames and then kills itself with `SIGKILL` gets `DecodeEnded` with 20,480 frames and `Failed`. A track that finishes decoding before the engine answers is still preceded by its `FileLoaded` (the fake worker is told to decode at once, and the test delays nothing). A client that disconnects before decoding ends causes no error in `kool_spin.log` and no stuck worker. (test: "Supervisor unload: DecodeEnded")
- [ ] `[auto]` **Refusals:** an `UnloadDeck` with deck 4 closes the connection after an `Error` `BadMessage`, as `decode()` refuses. A client that sends `DecodeEnded`, `DeckUnloaded`, or the engine's `UnloadDeck` reply gets `Error` `NotSupported` and stays connected. (test: "Supervisor unload: what it refuses")

The client library (`session_test.cpp`), with `kool_spin` running the null backend:

- [ ] `[auto]` **Connecting:** `Session::connect()` returns a session whose snapshot has a growing heartbeat. A runtime directory with no socket returns the not-reachable error; a socket owned by another user ID (the test passes a different `expected_uid` through the library's test hook, `connect_as`) returns the same. `Session::connect()` with a name of 65 bytes returns an error before it connects. (test: "Session: connecting")
- [ ] `[auto]` **A ring and a command:** after `open_ring()` (slot 0, which the status block already shows attached), `set_playing(0, true)` on a loaded deck is applied (`wait_applied()` returns applied) and the deck's position advances. Sequence numbers start at 1 and increase. A second `open_ring()` returns an error. `send()` before `open_ring()` returns an error and writes nothing. (test: "Session: a ring and a command")
- [ ] `[auto]` **Rejected commands:** `set_rate(0, NaN)`, `set_crossfader_assign(0, 9)`, and `set_playing(7, true)` are each reported by `wait_applied()` as rejected, naming `bad value`, `bad value`, and `bad target`; commands after them are still applied; and a new session's ring doesn't see the earlier session's rejections. (test: "Session: rejected commands")
- [ ] `[auto]` **The snapshot:** after loading the FLAC on deck 1 and sending a rate, a fader, a crossfader assignment, a main level, and a bus map, the snapshot shows each value, the deck's track ID, rate, and `kDeckComplete`, and the session's slot with `commands_read` and `last_sequence` equal to what was sent. A state segment the test fills with an invalid output state, side, and rejection code (the library's test hook `snapshot_from` reads a block from a given mapping) makes `snapshot()` return an error naming that field; NaN and infinities in reals are kept. A seqlock that the test leaves odd returns an error naming the block, in under 100 ms. (test: "Session: the snapshot")
- [ ] `[auto]` **Requests and notifications:** `load_file()` of the FLAC returns its `FileLoaded`; the `DecodeEnded` that follows comes from `next_decode_ended()`, and a second call times out. Two requests the test makes while a `DecodeEnded` is in flight each get their own reply. `ping()` and `unload_deck()` work. `stop()` returns once `kool_spin` has exited with status 0, and every call after it returns the closed error without blocking. (test: "Session: requests and notifications")

`ks-play` (`play_test.cpp`):

- [ ] `[auto]` **Loading and playing:** against a running `kool_spin`, `ks-play load 1 <flac>` exits 0 with the line `loaded track <id> on deck 1: 44100 Hz, estimated 44100 frames` and nothing on standard error; a relative path gives the same result; `ks-play play 1` prints `ok` and exits 0, and a second `status` shows deck 1 playing with its position advanced. `ks-play pause 1` stops it. `ks-play load 0 <flac> --wait` prints the line, then `decoded 44100 frames: complete`, and exits 0; with a track that fails (the fake worker's `Failed` ending), it prints `decoded 20480 frames: failed` and exits 1. (test: "ks-play: load, play, pause")
- [ ] `[auto]` **Every control command:** each command in requirement 22's table, with valid arguments, prints `ok` and exits 0, and the next `status` shows its value: rate 1.02, seek to frame 22,050, cue point 11,025, trim −3, fader 0.5, assign `b`, channel cue `on`, crossfader 0.25, curve 0.5, main level −6, headphone level −9, cue mix 0.75, and bus map 2 and 4. `trim`, `main-level`, and `headphone-level` accept `-inf`. (test: "ks-play: every control command")
- [ ] `[auto]` **Rejections:** `ks-play rate 0 nan` is a usage error (status 2). `ks-play bus-map 1 2` parses, but the engine rejects odd channels: it exits 1, prints nothing on standard output, and its standard error starts `ks-play: ` and contains `bad value`. `ks-play rate 0 -1` exits 0 and prints `ok`, since the engine clamps out-of-range values instead of rejecting them (`specs/commands/spec.md` requirement 4); and so does `ks-play play 3` on a deck with no track. (test: "ks-play: rejections")
- [ ] `[auto]` **Unloading and stopping:** `ks-play unload 1` prints `unloaded deck 1` and exits 0; the next `status` shows deck 1 with `loaded=0`. `ks-play ping` prints `pong`. `ks-play stop` prints `stopped`, exits 0, and `kool_spin` has exited with status 0 within 5 s. (test: "ks-play: unload, ping, stop")
- [ ] `[auto]` **Status:** the first line of `ks-play status` starts `engine heartbeat=`; there are exactly 1 + 4 + 4 + 1 lines of `engine`, `deck 0` to `deck 3`, `channel 0` to `channel 3`, and `mixer`, followed by one `client` line for the session's own slot (the test's own ring included), in that order; each line has the fields requirement 25 lists, in that order, with no others; `state=playing` with the null backend; `side` is one of `a`, `b`, `thru`. The heartbeat in a second run is greater than in the first. Standard error is empty. (test: "ks-play: status")
- [ ] `[auto]` **Watching:** `ks-play status --watch --interval 100` prints at least five snapshots in 1 s, separated by `---`, with growing heartbeats; `SIGINT` ends it with status 0 within 1 s. With `kool_spin` killed during a watch, it exits with status 1 and a line starting `ks-play: ` on standard error. (test: "ks-play: watching")
- [ ] `[auto]` **Failures to connect:** with no `kool_spin`, any command exits 3 with a line starting `ks-play: ` on standard error and nothing on standard output, in under 1 s. A runtime directory with a stale socket file does the same. (test: "ks-play: no kool_spin")
- [ ] `[auto]` **Errors from `kool_spin`:** `ks-play load 0 /nonexistent` exits 1; standard error starts `ks-play: ` and contains `refused`. With four workers busy (the fake worker, as in `specs/track-loading/spec.md`'s test), a fifth `load` contains `unavailable`. (test: "ks-play: errors from kool_spin")
- [ ] `[auto]` **Usage:** each of these exits with status 2, with a line starting `ks-play: ` on standard error, nothing on standard output, and without connecting (no runtime directory is needed): no command; an unknown command; a missing argument and an extra argument for each command; deck 4; deck `-1`; deck `1.5`; deck ` 1`; a real with trailing characters (`1x`); hexadecimal (`0x10`); `inf` and `nan` for a command that doesn't allow them; `-inf` for `rate`; an empty path; an unknown `assign` side; `channel-cue maybe`; `--interval 49` and `--interval 60001`; a repeated `--runtime-dir`; and a name of 65 bytes. `--help` exits 0 with the usage on standard output. (test: "ks-play: usage")
- [ ] `[auto]` **Interrupted:** `SIGTERM` to a `load` waiting on a slow probe (the fake worker) ends `ks-play` with status 143 within 1 s, and `kool_spin` goes on running and serves the next client. (test: "ks-play: interrupted")
- [ ] `[auto]` **Fuzzed:** `ks_play_args_fuzz` runs 1,000,000 inputs clean under the `fuzz` preset. (test: `ks_play_args_fuzz`)

Everywhere:

- [ ] `[auto]` **The earlier steps' tests** all still pass. Three protected test helpers change, because a `DecodeEnded` can now arrive between a client's request and its answer: `load()` in `tests/acceptance/track-loading/tracks_support.h`, `open_ring()` in `tests/acceptance/client-rings/rings_support.h`, and one read in "Supervisor tracks: the latest load wins" skip it. No existing test used types 24 to 26 as unknown.
- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux in every preset, and on macOS arm64 in `debug`, `release`, `asan`, and `tsan`.
- [ ] `[manual]` **By hand:** the maintainer runs `kool_spin` with the real backend on the Inpulse 500, and with `ks-play` loads two tracks, plays them, moves the crossfader, and watches `ks-play status --watch` while the audio plays, with no underruns caused by `ks-play` running.

## Out of scope

- **Interactive use and scripting:** a REPL, scripts, and batching several commands over one connection. One run is one command.
- **Starting `kool_spin`,** and any way to find or stop an orphaned supervisor or engine (step 8).
- **Health checks** (real-time priority, memory locking, power profile, battery): step 10 reports them, through `ks-play`, once they exist.
- **Logs for clients:** a client's own log segment (`specs/logs/spec.md`).
- **Playlist and library commands,** and anything the UI will need beyond what is here (a UI's connection handling, queuing, and threading).
- **Reading the recording** that step 7's chaos tests use.
- **More than one ring per session,** and closing a ring without closing the connection.
- **Windows.**

## Open questions

The maintainer approved these decisions on 2026-11-18, after discussing decision 2 (one-shot runs cost `ks-play` tens of milliseconds of setup, which doesn't matter for a diagnostic tool; real clients keep a session and a ring open, and controllers run inside the engine). Batching several commands in one run, and a persistent mode, are deferred:

1. **Message design.** `UnloadDeck` is a message, not a ring command, as the track-loading spec deferred it: unloading releases a track, which only the supervisor can finish. `DecodeEnded` goes only to the client that asked for the load, and only while it's connected. The alternative is a subscription message for every client.
2. **One command per run.** Each control command opens a ring, waits for the engine to apply it, and closes the ring, so `ks-play` costs one ring slot for a few milliseconds. A stuck command shows up as a timeout rather than being dropped silently.
3. **`ks::client` as its own library.** The UI will need the same session and snapshot code, so it isn't buried in `src/apps/play/`. That is a new directory and library, but no new dependency.
4. **Test hooks.** `Session::connect_as` (a different expected user ID) and `snapshot_from` (read from a mapping the test supplies) are public, documented as test hooks, because the corrupt-segment and wrong-owner cases can't be made any other way.
5. **The `status` format** is plain `name=value` lines, which tests, scripts, and `grep` can read. There's no JSON output yet.
