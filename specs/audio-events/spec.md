# Feature: The audio thread's events

- **Status:** Approved by the maintainer on 2026-11-16. Protected: build loops can't change it.
- **Roadmap:** Phase 1b, step 4b
- **Built by:** interactive

## Goal

Log what goes wrong while the engine renders: late blocks, non-finite samples, rejected commands, corrupt client rings, and decks waiting for decoding. The audio thread can't log, so it writes small fixed-size events to a private lock-free ring. The control thread turns them into log lines, summarized once a second, so an overloaded engine can't flood its log. `ks-engine` also stops handling signals asynchronously, so no signal handler can run on the audio thread, or inside real-time code.

## Context

- **ADRs that apply:**
  - **ADR 0003 §13:** the audio thread writes fixed-size records (an event code and a few numbers) to a private lock-free ring, and the control thread formats them into the log.
  - **ADR 0003 §1 and §2:** one render function, called by whichever driver holds the render token: the audio thread. The control thread isn't real-time; it builds and frees objects, and runs the output.
  - **ADR 0003 §10:** objects pass between the two threads through in-process rings, the give ring and the retire ring. The event ring is a third.
- **`CLAUDE.md`:** code reachable from the audio callback must not log, lock, allocate, or make system calls. A signal handler that interrupts `KS_NONBLOCKING` code runs as real-time code under RealtimeSanitizer, so a process blocks the signals it handles while it calls such code, and keeps them blocked on the audio thread.
- **What exists:**
  - **`RenderCore`** counts late blocks (render time longer than the block lasts) and the non-finite samples the safety stage silenced, and passes them, with the block's render time and load, to `Engine::rendered()` as `RenderStats` after every block it renders.
  - **`Engine`** counts, for each client slot, the commands it read and rejected, with the last rejection's reason, and whether the ring was found corrupt (`ClientStats`). `Deck::waiting()` says whether a deck is held at the end of the frames decoded so far. All of these are published in the state segment, and none is logged.
  - **`ks-engine`** logs through its log segment (`specs/logs/spec.md`). Its control thread wakes at least every 10 ms (`kControlInterval`; `specs/processes/spec.md` requirement 10). Its handlers for `SIGTERM`, `SIGINT`, and `SIGHUP` write to a self-pipe, and can run on any thread, the audio thread included. PR #30 found the same pattern failing under RealtimeSanitizer in `kool_spin`, and fixed it there.
- **This spec extends** `specs/logs/spec.md` requirement 18 (the engine's lines and levels) and `specs/processes/spec.md` requirements 10 and 11 (what the control thread does, and how a signal stops the engine). The rest of those specs still applies unchanged.
- **The API exists before the code:** the acceptance tests compile against the public headers. Until the maintainer approves this spec and the tests, the function bodies are stubs, and every acceptance test fails.

## Requirements

### Events

1. **`AudioEvent`,** in a new header, `<ks/engine/audio_events.h>`: a trivially copyable record of one thing that happened in one block.

   | Field | Type | Meaning |
   |---|---|---|
   | `kind` | `AudioEventKind` | `LateBlock`, `NonFinite`, `Rejected`, `CorruptRing`, `Waiting`, or `WaitEnded` |
   | `index` | `std::uint32_t` | The client slot (`Rejected`, `CorruptRing`) or the deck (`Waiting`, `WaitEnded`), from 0; 0 for the others |
   | `count` | `std::uint64_t` | How many: late blocks, samples silenced, or commands rejected; 1 for the others |
   | `sequence` | `std::uint64_t` | `Rejected`: the sequence number of the last slot rejected; 0 for the others |
   | `rejection` | `Rejection` | `Rejected`: the last slot's reason; `UnknownType` for the others |
   | `load` | `double` | `LateBlock`: the block's render load; 0 for the others |
   | `duration` | `std::chrono::nanoseconds` | `LateBlock`: the block's render time. `WaitEnded`: how long the deck waited, from the host time of the block in which it started waiting to this block's. 0 for the others |
   | `host_time` | `std::chrono::steady_clock::time_point` | The block's host time |

2. **What the audio thread records,** with or without a state block. `Engine::process()` records, in this order:
   - **`Rejected`:** one event for each client slot whose ring had at least one slot rejected in this block, with the number rejected in this block.
   - **`CorruptRing`:** one event when a slot's ring is first found corrupt. The engine stops reading it (`specs/commands/spec.md`), so this happens once for each ring attached.

   Then `Engine::rendered()` records, in this order, before anything it publishes:
   - **`LateBlock`:** when `RenderStats::late_blocks` is higher than in the last call. `count` is the increase, and `load` and `duration` are this block's `render_load` and `render_time`.
   - **`NonFinite`:** when `RenderStats::non_finite_samples` is higher than in the last call. `count` is the increase.
   - **`Waiting`:** for each deck, from deck 0 up, whose `waiting()` is true after this block and was false after the last one.
   - **`WaitEnded`:** for each deck, from deck 0 up, whose `waiting()` is false after this block and was true after the last one, whatever the reason: decoding caught up, or the deck was paused, moved, or unloaded.

   Before the first call, the last values are 0, and no deck was waiting.
3. **The event ring:** a private `ipc::SpscRing` of `AudioEvent`, with `kAudioEventCapacity` (1,024) slots, owned by the `Engine`. The audio thread is its only producer, and the control thread its only consumer. When it's full, the event is dropped and counted; the count only goes up. Recording an event allocates nothing, takes no lock, and makes no system call: everything it calls is `KS_NONBLOCKING`.
4. **The control thread's side,** on `Engine`:
   - `std::optional<AudioEvent> take_event() noexcept`: the oldest event not yet taken, or nothing. Control thread only, like `collect()`.
   - `std::uint64_t dropped_events() const noexcept KS_NONBLOCKING`: the events dropped so far because the ring was full. Any thread may call it.

### Lines

5. **`EventLines`,** in `<ks/engine/audio_events.h>`, turns events into log lines on the control thread. A line is an `EventLine`: an `ipc::LogLevel` and a `std::string`. Each call takes the time it's made, `now`, on the steady clock, and returns the lines to write now, in order:
   - `add(const AudioEvent&, now)`: one event, taken from the engine at `now`;
   - `dropped(std::uint64_t total, now)`: the engine's `dropped_events()`, read at `now`. An increase since the last call counts as one event with that count;
   - `tick(now)`: summaries for every stream whose window ended by `now`;
   - `flush(now)`: as `tick()`, and also a summary for every open window that counted anything, however short, for when the engine stops.

   `add()` and `dropped()` first do what `tick(now)` would.
6. **Streams and windows:** events belong to streams: late blocks; non-finite samples; rejections from slot n; corrupt rings in slot n; deck n waiting; deck n's waits ending; dropped events.
   - **A quiet stream:** its next event becomes a line at once, and opens a window of `kEventSummaryInterval` (1 s) starting at `now`.
   - **An open window** counts its stream's events, adding up their `count`s, and keeps the worst: the highest load, the longest wait, or the last rejection.
   - **When a window ends** (`now` is at or after its end): if it counted anything, its stream gets one summary line, and a new window starts where it ended. If it counted nothing, the stream goes quiet.

   So each stream writes at most its first line and one line a second. Summaries come in the order of the list above, by slot or deck number.
7. **Levels and wording:** decks are numbered from 1 in lines, as `ks-harness` numbers them, and slots from 0, as `specs/logs/spec.md`'s lines number them. Numbers are written as plain decimals. Where the table shows `…`, a summary for a full window says `in the last second`, and one that `flush()` writes for a shorter window says `in the last <s> s`, with three decimals.

   | Stream | Level | First line | Summary |
   |---|---|---|---|
   | Late blocks | `warning` | `late block: rendered in <ms> ms, load <load>` | `late blocks: <count> more …, highest load <load>` |
   | Non-finite samples | `warning` | `silenced <count> non-finite samples` | `non-finite samples: <count> more silenced …` |
   | Rejections from slot n | `warning` | `rejected <count> command(s) from slot <n>; the last: <reason>, sequence <seq>` | `rejected <count> more command(s) from slot <n> …; the last: <reason>, sequence <seq>` |
   | Corrupt ring in slot n | `error` | `the ring in slot <n> is corrupt; its commands are ignored` | `the ring in slot <n> was found corrupt <count> more time(s) …` |
   | Deck n waiting | `warning` | `deck <n> is waiting for decoding` | `deck <n> waited for decoding <count> more time(s) …` |
   | Deck n's waits ending | `info` | `deck <n> stopped waiting after <s> s` | `deck <n> stopped waiting <count> more time(s) …, the longest after <s> s` |
   | Dropped events | `warning` | `dropped <count> audio-thread events: the event ring was full` | `dropped <count> more audio-thread events …` |

   `command(s)` and `time(s)` are singular when the count is 1. `<ms>` has three decimals, `<load>` two, and `<s>` three. `<reason>` is `an unknown type`, `a bad target`, `nonzero padding`, or `a bad value`. The wording isn't part of the interface beyond the phrases in requirement 10.

### `ks-engine`

8. **Logging events:** on every pass of the control loop, after `OutputController::poll()`, the engine takes every event from `take_event()` and gives it to its `EventLines`, then gives it `dropped_events()`, then calls `tick()`. It writes each line returned, at its level, as it writes its other reports: to the log with `--log-fd`, and to standard error without. When it stops, after the output controller's `stop()` returns and before its `stopped` line, it does the same once more, with `flush()` in place of `tick()`. A line's time is when it's written: at most one pass of the control loop after its block.
9. **Signals:** `ks-engine` installs no signal handlers, and no longer has a self-pipe.
   - **Blocked everywhere:** in `run()`, before it creates any thread, it blocks `SIGTERM`, `SIGINT`, and `SIGHUP` with `pthread_sigmask()`. Every thread it creates, and every thread miniaudio creates, inherits the mask, so none of them ever runs anything on those signals. The mask stays until the process exits: restoring it would let a second signal, sent during the fade-out, end the engine by its default action rather than with status 0.
   - **Taken by the control thread:** on every pass of the control loop, it checks `sigpending()` for any of the three. If one is pending, it takes it with `sigwait()`, and stops as `specs/processes/spec.md` requirement 11 says. A signal that arrives while the engine is starting stops it at the loop's first pass.

### The lines tests look for

10. **Fixed phrases:** these lines contain these phrases, where `<n>` is a slot or deck number and `<count>` a plain decimal:

    | Level | Contains |
    |---|---|
    | `warning` | `late block` |
    | `warning` | `non-finite samples` |
    | `warning` | `rejected <count> command from slot <n>` or `rejected <count> commands from slot <n>`, or either with `more ` before `command` |
    | `error` | `the ring in slot <n> is corrupt` |
    | `warning` | `deck <n> is waiting for decoding` |
    | `info` | `deck <n> stopped waiting` |
    | `warning` | `audio-thread events` |

### Everywhere

11. **`CLAUDE.md`:** its real-time rules say `ks-engine` doesn't block its signals yet. That sentence changes to say that `ks-engine` installs no handlers: it blocks the signals in every thread and takes them on the control thread. This is a protected change, which this spec approves.

## Acceptance criteria

The `Engine` tests are in `tests/acceptance/audio-events/`. They run the engine on the test's thread, as the offline driver does, so under the `rtsan` preset RealtimeSanitizer checks every event the audio thread records.

- [ ] `[auto]` **Late blocks:** an engine built without a state block. `rendered()` is called four times, with `late_blocks` 0, 1, 1, and 3, and each call's own `render_load` and `render_time`. `take_event()` then returns two `LateBlock` events: the first with `count` 1 and the second call's load and render time, and the second with `count` 2 and the fourth call's. Then it returns nothing. An engine built with a state block records the same events. (test: "Audio events: late blocks")
- [ ] `[auto]` **Non-finite samples:** a render core renders, offline, an engine whose deck 0 plays a track with a NaN in every 100th frame, for 20 blocks of 512 frames. The `NonFinite` events' counts add up to the core's `non_finite_samples()`, which is more than 0, and their host times are those of blocks the core rendered. With a track of finite samples, no `NonFinite` event is recorded. (test: "Audio events: non-finite samples")
- [ ] `[auto]` **Rejected commands:** a ring attached in slot 2. In one block it holds five slots, sequences 1 to 5: two valid ones, then an unknown type, then a valid one, then a bad value, with sequence 5. That block records one `Rejected` event, with `index` 2, `count` 2, `sequence` 5, and `BadValue`. The next block, with nothing in the ring, records none. (test: "Audio events: rejected commands")
- [ ] `[auto]` **A corrupt ring:** a ring attached in slot 1, whose head is then set 2,000 past its tail. The next block records one `CorruptRing` event with `index` 1, and the ten blocks after it none. After the ring is detached, its release collected, and another corrupt ring attached in slot 1, the next block records one more. (test: "Audio events: a corrupt ring")
- [ ] `[auto]` **Waiting for decoding:** deck 1 plays a 48 kHz track of 48,000 frames, with 9,600 published as ready, at 48 kHz in blocks of 480 frames. Exactly one `Waiting` event is recorded, with `index` 1, in the block where `waiting()` first becomes true. Five blocks later the track is published complete, and the next block records exactly one `WaitEnded` event, with `index` 1, whose `duration` is the difference between the two events' host times. Nothing else is recorded. (test: "Audio events: a deck waiting for decoding")
- [ ] `[auto]` **A full ring:** with nothing taken, 1,100 calls to `rendered()`, each with `late_blocks` one higher, record 1,024 events, and `dropped_events()` is 76. `take_event()` returns them in the order recorded: the first 1,024. After that, the next late block is recorded again, and `dropped_events()` stays 76. (test: "Audio events: a full ring drops and counts")
- [ ] `[auto]` **Once a second:** an `EventLines`. A `LateBlock` event at time 0 returns one `warning` line containing `late block`, with its load. 36 more, between 0 and 0.9 s, the highest with load 1.83, return nothing, and so does `tick(0.99 s)`. `tick(1.0 s)` returns one `warning` line containing `late blocks: 36 more in the last second` and `highest load 1.83`. `tick(2.0 s)` returns nothing, and the stream is quiet: an event at 2.5 s returns a first line at once. (test: "Event lines: the first at once, then once a second")
- [ ] `[auto]` **Separate streams:** rejections from slots 0 and 3, interleaved, each get their own first line, and their own summary, slot 0's first. A `CorruptRing` event in slot 0 between them gets its own `error` line at once. (test: "Event lines: each stream on its own")
- [ ] `[auto]` **Levels and wording:** one event of each kind, each in its own `EventLines`, returns one line, with the level and the phrase requirements 7 and 10 give. Deck index 0 is written `deck 1`. `count` 1 is written `rejected 1 command`, and 2 `rejected 2 commands`. A `WaitEnded` of 420 ms says `after 0.420 s`. (test: "Event lines: levels and wording")
- [ ] `[auto]` **Dropped events and flushing:** `dropped(0, 0)` returns nothing, and `dropped(76, 0.1 s)` returns a `warning` containing `dropped 76 audio-thread events`. `dropped(100, 0.5 s)` returns nothing. `flush(0.6 s)` returns a line containing `dropped 24 more audio-thread events in the last 0.500 s`. (test: "Event lines: dropped events and flushing")
- [ ] `[auto]` **Rejected commands reach the log:** `ks-engine`, started with `--log-fd` by the test as its supervisor, with a ring attached in slot 0. A client writes one command of unknown type. Within 2 s the log segment has a `warning` line containing `rejected 1 command from slot 0`. The client then writes 2,000 more, as fast as the ring takes them. Within 4 s of the last being read, the lines containing `from slot 0` add up, by their counts, to 2,001, and there are at most 6 of them. (test: "Engine events: rejected commands, summarized")
- [ ] `[auto]` **A corrupt ring reaches the log:** as above, but the client sets the ring's head 2,000 past its tail. Within 2 s the log segment has an `error` line containing `the ring in slot 0 is corrupt`, and the engine still answers a `Ping`. (test: "Engine events: a corrupt ring")
- [ ] `[auto]` **Flushed when it stops:** as in "rejected commands reach the log", the client writes 2 commands of unknown type and, once the first line is in the log, 50 more. As soon as all 52 have been read, well within a second of the first line, so its window is still open, the test sends `SIGTERM`. The engine exits with status 0, and the log's lines containing `from slot 0` add up, by their counts, to 52, all before its `stopped` line. (test: "Engine events: flushed when it stops")
- [ ] `[auto]` **Signals blocked in every thread:** on Linux, while `ks-engine` plays on the null backend, every thread listed in `/proc/<pid>/task` has `SIGTERM`, `SIGINT`, and `SIGHUP` in its `SigBlk` mask, and none of them in `SigCgt`. Then `SIGTERM` ends the engine with status 0. On macOS, where no other process can read a thread's mask, the test is skipped. (test: "Engine process: signals blocked in every thread")
- [ ] `[auto]` **The earlier steps' tests** all still pass, including "Engine process: signals fade out and exit", which checks that each of the three signals stops the engine with a fade-out.
- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux in every preset, and on macOS arm64 in `debug`, `release`, `asan`, and `tsan`.

## Out of scope

- **The limiter's activity:** limiting is the safety stage working as designed, not a fault. The state segment still counts limited frames.
- **The device's own underruns:** miniaudio doesn't report them the same way on every backend. A late block is the engine's own measure: render time longer than the block lasts.
- **Step 5:** the decode worker, and tracks in `ks-engine`. Until then, nothing in `ks-engine` can wait for decoding, and its decks play nothing.
- **Step 6:** `ks-play`, which will show these counts live from the state segment.
- **Step 7:** watching the heartbeat, which finds an audio thread that has stopped altogether.
- **Step 10:** health checks, such as real-time priority, which make late blocks less likely.
- **`kool_spin`'s signals:** it keeps its self-pipe and blocks its signals while it reads shared memory (PR #30). It's single-threaded (ADR 0003 §6), and has no audio thread.
- **`ks-harness`:** its engine records events too, but nothing takes them, so they're dropped and counted. Its report already gives the counts.

## Open questions

None. The maintainer approved these decisions; the first two are the maintainer's answers of 2026-11-16:

1. **No signal handlers in `ks-engine`** (requirement 9): with every thread blocking the signals, nothing runs asynchronously, so no handler can run on the audio thread or inside `KS_NONBLOCKING` code, and there's nothing to keep blocked around particular calls. The control loop already wakes at least every 10 ms, so a stop starts as soon as before.
2. **Summaries once a second** (requirement 6): the first event of a stream is logged at once, then at most one line a second per stream. About 20 streams can be busy at once: late blocks, non-finite samples, 8 slots' rejections, 4 decks starting and 4 ending waits, and dropped events. That's about 20 lines a second at most, which the 1,024-line log ring holds easily. At that rate a 16 MiB file fills in about two hours; with late blocks alone, one line a second, in about two days.
3. **Events come from what the engine already counts** (requirement 2): the render core doesn't change. `Engine::rendered()` compares the stats it's given with the last ones, and the deck's waiting flag with the last block's.
4. **One event per stream per block** (requirement 2): a block's rejections from one slot are one event, so a block records at most 1 + 1 + 8 + 8 + 4 = 22 events, whatever the clients send: a deck starts or stops waiting in a block, not both. At 32-frame blocks at 48 kHz, the control thread's 10 ms holds 15 blocks: 330 events at the very most, within the ring's 1,024.
5. **Lines are stamped when written** (requirement 8): `LogWriter` stamps each line under its mutex, which keeps the ring in time order. An event's line is at most one pass of the control loop, about 10 ms, after its block.
6. **Levels** (requirement 7): a corrupt ring is an `error`, because that client's commands are lost from then on, as `kool_spin` logs a corrupt log ring as an error. A deck that stops waiting is `info`. The rest are warnings: the audio thread coped, but something was audible or wrong.
7. **Decks from 1, slots from 0** (requirement 7): decks as on hardware and in `ks-harness`; slots as in `specs/logs/spec.md`'s lines and the protocol's messages.
8. **No `[manual]` criterion:** in this step `ks-engine` plays no tracks, so it renders silence quickly and nothing waits for decoding. Real-device checks come with steps 5 and 6, when it plays tracks and `ks-play` drives it.
