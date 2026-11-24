# Feature: Hang detection (step 7b)

- **Status:** Approved by the maintainer on 2026-11-23. Once the maintainer approves it, this file is protected: build loops can't change it.
- **Roadmap:** Phase 1b, step 7b
- **Built by:** interactive

## Goal

The supervisor kills and restarts an engine that is still running but not working. If the audio thread stops making progress (the engine-status heartbeat doesn't change for 500 ms), the supervisor sends `SIGKILL`, and the kill goes through 7a's restart path unchanged. If only the control thread stops answering pings, the supervisor reports it, and kills the engine only once no deck is playing, so working audio is never interrupted.

## Context

- **ADRs that apply:** ADR 0003 §11, "Hangs" (the heartbeat counts rendered blocks and every driver advances it, freewheel included; 500 ms without it means `SIGKILL`; an unanswered ping is reported, and the engine is restarted only once no deck is playing). ADR 0002's rule that nothing waits on the engine: the supervisor only reads shared memory and sends messages. ADR 0003 §8: copy, validate, use.
- **What exists:**
  - **`EngineStatus::heartbeat`** (`src/ipc/include/ks/ipc/state.h`), in the state segment's status seqlock, advanced after every rendered block. The supervisor already maps the state segment.
  - **`Ping` and `Pong`**: the engine answers `Ping` on its control thread (`specs/processes/spec.md` requirement 10).
  - **`DeckState`** in the state segment, with a playing flag per deck.
  - **Engine restart** (`specs/engine-restart/spec.md`): an engine that ends while the supervisor is `Running` is reaped, reported, and restarted, with the exit counted by the crash-loop guard.
- **The maintainer's design choices** (2026-11-23):
  - poll the heartbeat every 100 ms; kill at 500 ms with no change;
  - ping every 1 s; a control thread is unresponsive after 3 s with no `Pong`;
  - tests hang a real engine with `SIGSTOP`; the control-thread-only case gets a paced-backend test option;
  - test-only supervisor options `--hang-timeout <ms>` and `--ping-interval <ms>`, as `--crash-window` and `--ready-timeout` in 7a.
- **This spec extends** `specs/processes/spec.md` requirement 20 (an engine that ends is restarted; here, an engine can be made to end). Everything else in the earlier specs still applies.

## Requirements

### The heartbeat watch

1. **When it runs:** from the engine's `Ready` until it ends. The supervisor doesn't watch a starting engine (`kEngineReadyTimeout` does that), a stopping one, or one it is already killing.
2. **Polling:** the supervisor reads `EngineStatus` through the state segment's seqlock every `kHeartbeatPoll` (100 ms), from its event loop. It copies the status and uses only the copy. A read that doesn't succeed (a writer was mid-write) is retried at the next poll and doesn't count as a stall.
3. **What counts as a stall:** `heartbeat` equal to the last value read. The supervisor records the monotonic time at which it last saw the value change; at `Ready` that time is the time of `Ready`, and the last value is the one read then. A value that goes backward counts as a change (a restarted engine counts from its resume; the supervisor resets its record at each `Ready`).
4. **The kill:** when the heartbeat hasn't changed for `kHangTimeout` (500 ms), the supervisor reports `ks-engine heartbeat stopped for <n> ms; killing it`, sends `SIGKILL` to the engine, and carries on as for any exit: it reaps the process, reports `ks-engine killed by signal 9`, and restarts it as 7a's requirement 2 says. The exit counts once in the crash-loop guard.
5. **Detection time:** the supervisor kills at most `kHangTimeout + kHeartbeatPoll` (600 ms) after the last heartbeat change it saw, plus the time its own event loop is delayed.
6. **Every driver advances the heartbeat** (ADR 0003 §11), so a paused deck, an empty engine, a freewheeling engine (device loss), and a paced engine are not hung. The supervisor treats them like any other.
7. **After the kill:** the restart is 7a's, unchanged: the same segments, tracks, and ring slots, the resume at the position the decks would have reached, a 50 ms fade-in, and the restart time reported.

### Pings

8. **Sending:** while the engine is `Ready` and not being killed, the supervisor sends the engine a `Ping` every `kEnginePingInterval` (1 s), with a new non-zero request ID, and only if no earlier ping of its own is still unanswered. It records the time of the oldest unanswered ping.
9. **Answers:** a `Pong` with a matching ID clears the record. A `Pong` that matches nothing is ignored.
10. **Unresponsive:** when the oldest unanswered ping is `kEnginePingTimeout` (3 s) old, the supervisor reports once `ks-engine control thread not answering`, and the engine is *unresponsive*. A later `Pong` ends it, and the supervisor reports `ks-engine control thread answering again`.
11. **Killing an unresponsive engine:** while the engine is unresponsive, at each poll the supervisor reads the deck states from a seqlock copy. If no deck is playing (the playing flag clear on every deck), it kills the engine as requirement 4 says, with `ks-engine control thread not answering for <n> ms and no deck is playing; killing it`. If any deck is playing, it does nothing, and checks again at the next poll. Decks that can't be read count as playing.
12. **Pings across restarts:** the record of unanswered pings is cleared when an engine ends and at each `Ready`. A ping isn't sent to an engine that is starting or restarting.
13. **A ping doesn't wait:** the supervisor never blocks on a ping or a `Pong`. Requests from clients are answered as usual while the engine is unresponsive; those that need the engine are answered `Unavailable`, text `ks-engine is not answering`, after the usual request timeout.

### Options and constants

14. **Test options** of `kool_spin`, documented in `--help` as for tests: `--hang-timeout <ms>` (default 500), `--ping-interval <ms>` (default 1000; the unresponsive limit is three times it). Zero or a non-number is a usage error (status 2). The poll interval is a fifth of the hang timeout, at least 10 ms.
15. **A paced-only engine option** `ks-engine --backend paced --hang-control-after <ms>`: after that many milliseconds from `Ready`, the control thread stops answering anything, and sleeps in a loop. It stops the control thread only; the render thread and the heartbeat carry on. Any other backend refuses it as a usage error (status 2), as `--start-delay` does.

### Everything else

16. **Real-time rules:** nothing in this step runs on the audio thread. The engine's only change is requirement 15's option, on the control thread.
17. **No new dependencies, no new message types.** ADR 0003 §11 gains a line for the thresholds and the options; no other ADR changes.
18. **Failures:** a supervisor that can't send `SIGKILL` (the process is already gone) carries on to reaping; a failed read of the state segment is a missed poll, never a kill.

## Acceptance criteria

All process tests use the paced backend with recording, a 10-minute generated track, and a runtime directory of their own. On Linux, `SIGSTOP` and `SIGKILL` go to the engine's process ID (from its lock file). Every test that starts `kool_spin` is blocked in the build-loop sandbox: it runs outside it (`CLAUDE.md`).

- [ ] `[auto]` **Stalled heartbeat is killed and resumes:** a track plays on deck 0 at rate 1. The engine gets `SIGSTOP`. The supervisor kills it, the process ID changes, and the log shows `heartbeat stopped`, `killed by signal 9`, and `restarted in <n> ms`. The detection time (stop to kill) is printed and is within 1.5 s with the defaults, within 4 s under `asan` and `tsan`. The recording continues, and the new engine's first frames are the track at the position it would have reached, within 10 ms, rising over 50 ms, as in the engine-restart "Restart: kill -9 resumes at the right position" test. The deck is playing with the same track ID. (test: `tests/acceptance/hang-detection/hang_test.cpp`, "Hang: a stalled heartbeat is killed and the deck resumes")
- [ ] `[auto]` **Mid-hang state comes back:** two tracks, the crossfader at 0.5, a cue point, and two clients with rings; after the kill, the decks, mixer, cue point, and ring slots are as in the engine-restart "Restart: decks, mixer, cue point, and rings come back" test. (test: `hang_test.cpp`, "Hang: decks, mixer, cue point, and rings come back")
- [ ] `[auto]` **Healthy engines are left alone:** over 5 s with `--hang-timeout 500`, an engine with a playing deck, an engine with every deck empty, and an engine with every deck paused each keep their process ID, and the supervisor reports no kill. (test: `hang_test.cpp`, "Hang: a healthy engine is never killed")
- [ ] `[auto]` **A stop that ends in time doesn't count:** `SIGSTOP`, then `SIGCONT` after 300 ms (well under the timeout): the engine keeps its process ID and the supervisor reports no kill. (test: `hang_test.cpp`, "Hang: a short stall is not a hang")
- [ ] `[auto]` **A starting engine isn't watched:** with `--start-delay` longer than the hang timeout (and shorter than `--ready-timeout`), the engine isn't killed for a stalled heartbeat, and comes to `Ready`. The watch starts at `Ready`. (test: `hang_test.cpp`, "Hang: a starting engine is not watched")
- [ ] `[auto]` **The guard counts hang kills:** three hangs within the crash window give the third as a clean restart (decks empty and paused, ring slots unchanged); a fourth gives up with status 3, as the engine-restart "Restart: the crash-loop guard" test says. (test: `hang_test.cpp`, "Hang: kills count toward the crash-loop guard")
- [ ] `[auto]` **Unresponsive control thread, a deck playing:** a paced engine with `--hang-control-after 500` and a playing deck: the supervisor reports `control thread not answering` once, the engine's process ID doesn't change over 3 further ping limits, the recording has no gap (the 440 Hz sine rule of the engine-restart chaos test), and a `Ping` from a client is still answered by the supervisor. (test: `hang_test.cpp`, "Hang: an unresponsive control thread with a playing deck is left running")
- [ ] `[auto]` **Unresponsive control thread, no deck playing:** the same engine with every deck paused: the supervisor kills it once the ping limit passes and restarts it, with the log saying `no deck is playing`. A second run pauses the deck *after* the engine has become unresponsive, and the kill follows at the next poll, within one poll after the pause is applied (the pause goes through a client's ring, which the hung control thread isn't needed for). (test: `hang_test.cpp`, "Hang: an unresponsive control thread is killed once no deck plays")
- [ ] `[auto]` **A late answer recovers:** the control thread is stopped with `SIGSTOP` for 1.5 pings and then resumed (`SIGCONT`); the supervisor reports no unresponsive engine. A stop for longer than the limit, resumed while a deck is playing, reports `answering again` and doesn't kill. (test: `hang_test.cpp`, "Hang: a late Pong ends the report")
- [ ] `[auto]` **Pings don't cross restarts:** after a kill, the first `Ping` to the new engine is sent at least one interval after its `Ready`, and no unanswered record carries over. (test: `hang_test.cpp`, "Hang: ping state is reset at each Ready")
- [ ] `[auto]` **Options:** `--hang-timeout 0`, `--ping-interval abc`, and `--hang-control-after` on a non-paced backend are usage errors with status 2; `--help` lists the new options. (test: `hang_test.cpp`, "Hang: options")
- [ ] `[auto]` **Nothing leaks:** after the tests above, no engine, worker, or client process is left, and the supervisor's descriptors are back to their count before the first kill, apart from the new engine's socket. (test: `hang_test.cpp`, "Hang: descriptors and processes don't leak")
- [ ] `[auto]` **The earlier steps' tests** all pass.
- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux in every preset, and on macOS arm64 in `debug`, `release`, `asan`, and `tsan`. (`SIGSTOP`, `SIGCONT`, and `SIGKILL` work on both; the timing thresholds for sanitizers are as stated.)
- [ ] `[manual]` **By hand:** on the Inpulse 500, the maintainer plays two tracks through `ks-play`, stops the engine with `kill -STOP`, and hears the music cut for about a second at most, then come back at the right place with a fade-in and no click.

## Out of scope

- **Supervisor crash and relaunch** (step 8), the session journal (step 9), and health checks.
- **Hung workers and hung clients:** a stuck decode worker is the decode-worker spec's decision 4.
- **Hang detection in the UI or the controller scripts,** and restarting a hung process other than the engine.
- **A hang of the supervisor itself:** nothing watches the supervisor in this step.
- **Detecting a device that stops calling back:** the engine's control thread treats it as device loss (ADR 0003 §11); the supervisor doesn't.
- **A watchdog inside the engine,** or a thread in the supervisor devoted to the watch: it runs in the supervisor's event loop.
- **Timing guarantees under load:** 600 ms is a target. The thresholds in the criteria catch regressions only.

## Open questions

None. The maintainer settled these on 2026-11-23:

1. **Test-only supervisor options** (`--hang-timeout`, `--ping-interval`), as in 7a, not environment variables. To be revisited if the option list grows.
2. **A stalled heartbeat is always a kill**, even when the deck states can't be read. For pings, unreadable decks count as playing.
3. **Requirement 13 stays in 7b:** requests that need an unresponsive engine are answered `Unavailable`, text `ks-engine is not answering`.
