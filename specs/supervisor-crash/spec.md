# Feature: Supervisor crash (step 8)

- **Status:** Approved by the maintainer on 2026-11-24; requirement 1's retries changed the same day, with the maintainer's approval, from 5 tries 10 ms apart, which failed the probing criterion about once in 100 runs. Protected: build loops can't change it.
- **Roadmap:** Phase 1b, step 8
- **Built by:** interactive

## Goal

When `kool_spin` dies, the music plays on, and clients that already have rings keep control. When `kool_spin` is launched again, it reports the engine that is still running and offers to stop it; if the user agrees, the engine fades out and a new session starts. No path signals a process that isn't that engine, and none starts a second engine in the same runtime directory.

## Context

- **ADRs that apply:** ADR 0002 §1 (the supervisor's row: its children keep running, and the supervisor depends on the standard library only). ADR 0003 §12 (the engine plays on; clients with rings keep control; a relaunch finds a live engine by its lock, doesn't start a second, and offers to stop the first; adoption is deferred). ADR 0003 §7: the kernel keeps a segment while anyone holds it.
- **What exists:**
  - **The engine** closes its supervisor's socket when it fails and plays on; `SIGTERM` fades it out (`specs/processes/spec.md` requirements 11 and 12). It leads its own process group, so closing the terminal it was started from doesn't end it.
  - **A worker** whose supervisor dies decodes its track to the end, then exits (`specs/decode-worker/spec.md` requirement 19).
  - **`kool_spin`** refuses to start while the `engine` lock is held, with status 1 and the process ID (`specs/processes/spec.md` requirement 15).
  - **Rings outlive the supervisor** for a client that holds its ring directly (`specs/client-rings/spec.md`).
- **What's wrong today:**
  - **`ks::client::Session`** unmaps its ring and the state segment at the first request that finds the connection closed.
  - **Each refused launch creates a session folder,** and only ten are kept, so ten refused launches remove the crashed session's logs.
  - **A lock file can hold an old process ID:** `acquire()` takes the lock before it writes its own ID.
  - **`ks-play`** says only `Connection refused`.
- **The maintainer's design choices** (2026-11-24):
  - a prompt on a terminal, with No as the default, and `--stop-previous` to say yes without being asked;
  - `Session` keeps its ring and its state mapping when the connection is lost;
  - `ks-play` says that an engine is running without `kool_spin`, but never stops it;
  - an engine without a supervisor never exits on its own.
- **This spec amends:** `specs/processes/spec.md` requirement 15 (step 3); `specs/logs/spec.md` requirement 12 (the order of starting); `specs/segments/spec.md` requirements 20 and 23; and `specs/ks-play/spec.md` requirements 13, 16, 19, 20, and 27. Everything else in them still applies.
- **Terms:** the *orphan* is the process that holds the `engine` lock when `kool_spin` starts. The text shown to users says "an engine from an earlier session", and "running" rather than "playing": a new supervisor has no state segment to tell.

## Requirements

### Lock files

1. **`acquire()` retries:** when another process holds the lock, `LockFile::acquire()` tries up to `kLockAttempts` (10) times, `kLockRetry` (5 ms) apart, before it fails. A probe holds a lock for microseconds, so it can no longer make an `acquire()` fail, and `ks-play` may probe (requirement 18).

### Finding the orphan

2. **`ks::supervisor::find_orphan(dir, wait)`** (`<ks/supervisor/orphan.h>`) probes the `engine` lock and returns: no orphan; an orphan with its process ID; an orphan whose ID isn't known; or the probe's error. It gives an ID only when two probes in a row, `kOrphanSettle` (100 ms) apart, read the same valid one, and it isn't the caller's own. It tries for up to `kOrphanSettleLimit` (1 s). `wait` sleeps, and is a parameter so that tests control it.
3. **`ks::supervisor::stop_orphan(dir, pid, wait)`** stops the orphan `find_orphan()` found:
   1. it sends `SIGTERM`, as requirement 4 says;
   2. it probes the lock every 10 ms for up to `kEngineStopTimeout` (2 s);
   3. if the lock is still held, it sends `SIGKILL`, as requirement 4 says, and probes for up to `kOrphanKillTimeout` (1 s).

   The orphan isn't the supervisor's child, so a free lock is what tells that it has ended. It returns: stopped, killed, already gone, or a failure with its reason.
4. **Signaling only the holder:** before each signal, it probes the lock again. If the lock is free, the orphan has gone, and nothing is signaled. If the lock names another process, or none, that's a failure, and nothing is signaled. On Linux, it opens a pidfd for the ID before that probe, and signals through the pidfd, so a reused ID can't be hit; if the kernel refuses pidfds, it uses `kill()`. On macOS it uses `kill()` straight after the probe.

### Launching

5. **The option:** `kool_spin --stop-previous` stops an orphan without asking. With no orphan it changes nothing. Given twice, it's a usage error. `--help` lists it.
6. **Order:** `kool_spin` takes the `supervisor` lock, then looks for an orphan, then opens its logs. A launch that ends here, because there is an orphan and the answer is no, creates no session folder.
7. **With an orphan,** it writes to standard error that an engine from an earlier session is still running, with its process ID, that `kool_spin` can't control it, and that stopping it fades out the music. Then:
   - **`--stop-previous`:** the answer is yes;
   - **on a terminal:** it asks `Stop it and start a new session? [y/N] ` on standard error, and reads one line from standard input. `y` or `yes`, in any case, with blanks around it ignored, is yes. Anything else is no, including an empty line, end of file, a read error, and a line longer than 64 bytes;
   - **otherwise:** the answer is no.

   It asks only if standard input and standard error are terminals, and its process group is standard input's foreground group, so a background launch never stops to read.
8. **No:** it writes that the engine was left running, and that `--stop-previous` stops it, and exits with status 1. Nothing is signaled.
9. **An orphan whose ID isn't known:** it says so and exits with status 1, without asking, whatever the options.
10. **While it asks,** no handlers are installed: `SIGINT`, `SIGTERM`, and `SIGHUP` end `kool_spin` and leave the orphan running. It holds the `supervisor` lock, so a second launch is refused as today.
11. **Yes: stop late.** It first does everything in starting that can fail without the orphan's being stopped: the logs, the worker and the budget, finding `ks-engine` and checking that it's an executable file, the segments, and the signal handlers. Only then does it stop the orphan (requirement 3), and carry on to start the engine. A launch that fails before that leaves the orphan running.
12. **The outcome:** one line on standard error, and the same in `kool_spin.log`:
    - **stopped,** or already gone: it carries on;
    - **killed:** a warning that the engine didn't stop in time and was killed, and it carries on;
    - **failed:** an error with the reason, and it exits with status 1 without starting an engine.
13. **Never two engines:** an orphan that takes its lock after requirement 6's probe is caught by the new engine's own `acquire()`, which fails the launch as today.

### The engine and workers

14. **No change.** The engine runs until a signal stops it, whatever its decks hold. Its log ring has no reader; what it logs after the crash fills the ring and is lost with it. Workers decode on to the end. What a worker that dies after the supervisor leaves unfinished stays `Decoding`, and its deck waits there in silence.

### The client library

15. **Losing the connection:** when a `Session` finds its connection closed or failed (`ErrorKind::Closed`), it closes the socket and keeps its ring, its state mapping, and any queued `DecodeEnded`:
    - **requests** (`open_ring`, `load_file`, `unload_deck`, `stop`, `ping`) return `Closed`, at once, from then on;
    - **`send`, `wait_applied`, and `snapshot`** work as before, on the mapped segments;
    - **`next_decode_ended`** returns what's queued, then `Closed`.

    `close()`, a `stop()` that succeeded, and a `Protocol` error still end the session and unmap everything.
16. **`Session::connected()`** is false once the connection is lost or the session is over. It doesn't read the socket: it says what the last call found.
17. **After the loss, `wait_applied()` also checks the ring:** a command counts as read only once the ring's own tail, as the engine publishes it, has passed the command too. A client the supervisor dropped has had its ring detached, and its slot may be another client's by then; its commands are never read, so it gets `TimedOut`, not that client's answer. A caller tells that an engine has gone by a heartbeat that stands still in `snapshot()`.

    `RingProducer` gains a way to read the ring's tail, checked against its head as `try_push()` does.

### ks-play

18. **The hint:** when `ks-play` can't reach `kool_spin`, it probes the `supervisor` lock, then, only if that's free, the `engine` lock. If the engine's is held, its line also says that an engine is running without `kool_spin`, with its process ID if the file gives one, and to run `kool_spin` to stop it. The exit status is 3, as today. A probe that fails changes nothing. `ks-play` never signals a process, and creates no files.

### Everything else

19. **Nothing runs on the audio thread.** No new messages, dependencies, or shared-memory layouts.
20. **The ADR:** ADR 0003 §12 says how the offer is made (requirements 5 to 8) and what "detecting survivors" relies on (requirements 2 to 4); it says that each child holds a lock file, and is corrected: only the engine does.

## Acceptance criteria

The tests are in `tests/acceptance/supervisor-crash/`. Each uses its own runtime and log directories, and kills whatever it started. Tests that start `kool_spin` can't run in the build-loop sandbox (`CLAUDE.md`). A test that needs the prompt runs `kool_spin` as the leader of a session whose controlling terminal is a pseudo-terminal; every other test gives it `/dev/null` as standard input.

Locks (`lock_test.cpp`):

- [ ] `[auto]` **Retry:** a thread holds the lock for 20 ms, and `acquire()` in the meantime succeeds. Against a lock that stays held, it fails with the holder's ID, within 500 ms. (test: "Locks: acquire retries")
- [ ] `[auto]` **Probes don't hurt:** with a thread probing without pause, 200 `acquire()`s in a row succeed. (test: "Locks: a probe never fails an acquire")

The orphan (`orphan_test.cpp`), with a helper process that holds the `engine` lock:

- [ ] `[auto]` **Finding:** no file, a free lock, a held lock, a held lock with an empty or invalid file, and one with the caller's own ID each give the result requirement 2 says. With `wait` rewriting the file, an ID that changes once gives the new one, and one that changes at every probe gives "not known" after 1 s of waits. (test: "Orphan: finding it")
- [ ] `[auto]` **Stopping:** a helper that exits on `SIGTERM` is stopped, and one that ignores it is killed after 2 s. (test: "Orphan: stopping it")
- [ ] `[auto]` **Never the wrong process:** the file names a bystander process while the helper holds the lock, and `wait` changes the file after the first signal, so that the check before `SIGKILL` sees another ID: the result is a failure, and the process named second is still running. A lock freed before the first signal gives "already gone", and the process its file names is still running. (test: "Orphan: never signals another process")

Playing on (`crash_test.cpp`), with the paced backend, `--recording`, and a generated track:

- [ ] `[auto]` **No gap:** a 440 Hz sine plays on deck 0 and `kool_spin` gets `SIGKILL`. Over the next 2 s the recording has no gap, by the engine-restart chaos test's rule, and the engine's process ID doesn't change. (test: "Supervisor crash: playback has no gap")
- [ ] `[auto]` **Mid-decode:** `kool_spin` is killed while a 10-minute track decodes: the deck's track becomes complete with every frame, and the worker exits. (test: "Supervisor crash: a track still decoding finishes")
- [ ] `[auto]` **A session keeps control:** a `Session` with a ring pauses, seeks, and plays a deck and moves the crossfader after the kill, each reported `Applied`; `ping()` returns `Closed`, twice; `connected()` goes from true to false at the first; and `send`, `wait_applied`, and `snapshot` still work after it. A rejected command is still reported. `close()` then makes all of them return `Closed`. (test: "Supervisor crash: a session keeps control")
- [ ] `[auto]` **A dropped client gets no one else's answer:** the test plays the supervisor and the engine for a `Session`, closes its connection, and then shows the slot's `ClientAck` far ahead, as another client's would be, with the ring's tail unmoved: `wait_applied()` returns `TimedOut`. Once the tail moves, it returns `Applied`. (test: "Supervisor crash: a reused slot isn't mistaken for ours")

Relaunching (`relaunch_test.cpp`), after `SIGKILL` to a `kool_spin` that plays a track:

- [ ] `[auto]` **Reports and leaves it:** without a terminal, `kool_spin` exits with status 1, its standard error has the engine's process ID and `--stop-previous`, the heartbeat keeps increasing, and the log folder is unchanged, after 12 launches in a row. (test: "Relaunch: without a terminal, the engine is left running")
- [ ] `[auto]` **`--stop-previous`:** the old engine's last status block is muted and silenced, and the recording ends in a fade to silence with no step; its process is gone, `kool_spin` runs with another engine, and a client is welcomed and loads a track. `kool_spin.log` has the line of requirement 12. With no orphan, the option changes nothing. (test: "Relaunch: --stop-previous stops it and starts a session")
- [ ] `[auto]` **The prompt:** on a terminal, `y`, `YES`, and ` yes ` stop the engine and start a session; an empty line, `n`, `yess`, a 100-byte line, end of file, and `SIGINT` leave it running, and the `supervisor` lock is then free. (test: "Relaunch: the prompt")
- [ ] `[auto]` **Not in the foreground:** started in a background process group of the terminal's session, `kool_spin` doesn't ask: it exits with status 1. (test: "Relaunch: a background launch doesn't ask")
- [ ] `[auto]` **Stop late:** `--stop-previous` with a missing `--worker`, a missing `--engine`, and an `--engine` that isn't executable each exit with status 1, and the heartbeat keeps increasing. (test: "Relaunch: a launch that can't start leaves the engine running")
- [ ] `[auto]` **A stuck engine:** the orphan gets `SIGSTOP`; `--stop-previous` kills it, warns, and starts a session, within 5 s. (test: "Relaunch: a stuck engine is killed")
- [ ] `[auto]` **An unknown holder:** the test holds the `engine` lock with an empty lock file: status 1, with and without `--stop-previous`, and no engine is started. (test: "Relaunch: an unknown holder")
- [ ] `[auto]` **Options:** `--stop-previous` twice is a usage error with status 2; `--help` lists it. (test: "Relaunch: options")
- [ ] `[auto]` **Nothing leaks:** after the tests above, no engine, worker, or supervisor process is left, and both locks are free. (test: "Relaunch: nothing is left behind")

`ks-play` (`play_hint_test.cpp`):

- [ ] `[auto]` **The hint:** with an orphan, `ks-play status` exits with status 3 and its line has the engine's process ID; with nothing running, with a runtime directory that doesn't exist, and with only a `supervisor` lock held, the line has no hint. In none of them does `ks-play` create a file. (test: "ks-play: an engine without kool_spin")

Everywhere:

- [ ] `[auto]` **The earlier steps' tests** all pass, unchanged.
- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux in every preset, and on macOS arm64 in `debug`, `release`, `asan`, and `tsan`.
- [ ] `[manual]` **By hand:** on the Inpulse 500, the maintainer plays two tracks, kills `kool_spin` with `kill -9`, and hears nothing change. Relaunching asks; No leaves the music playing, and Yes fades it out without a click and starts a session that loads and plays a track.

## Out of scope

- **Adopting the running engine** (ADR 0003 §12): deferred by the ADR.
- **The session journal** (step 9). It stops being written when the supervisor dies, so the positions it holds age while an orphan plays on; what to do about that is step 9's question.
- **A supervisor that hangs** without dying: nothing watches it.
- **An orphan that crashes or hangs:** nothing restarts it.
- **An orphan in another runtime directory, or in one that has been removed:** its lock can't be found, so a relaunch starts a second engine beside it. The directory comes from the environment and isn't permanent, so this can happen when:
  - the relaunch has another `XDG_RUNTIME_DIR` (Linux) or `TMPDIR` (macOS), or none, as from cron or a service, or is given another `--runtime-dir`;
  - on Linux, the user logs out of their last session, which removes `/run/user/<uid>`, while the orphan survives it, as one started over SSH can;
  - temporary-file cleanup removes the lock file, in `/tmp` or macOS's per-user temporary directory, or someone deletes the directory.

  Step 9 fixes it by moving the locks to the journal's folder (ADR 0003 §14), which is fixed and permanent, and leaving the socket where it is. Step 9 needs a lock there anyway, because two supervisors must not write one journal. It must decide how tests get a lock folder of their own, and what to do where `flock` is unreliable (network home directories). Requirements 2 to 4 take the folder as a parameter, so they don't change.
- **Keeping the orphan's log lines,** and an orphan that exits on its own.
- **Orphaned workers:** they end when their track does. A relaunch doesn't look for them.
- **Checking the new engine's options before stopping the orphan:** the supervisor doesn't parse what follows `--`.
- **A graphical offer,** when `kool_spin` is started from a desktop: Phase 3.

## Open questions

None. The maintainer settled the four choices above on 2026-11-24, and approved these, which follow from them, with the spec:

1. **Carry on after stopping:** yes starts a new session, rather than exiting.
2. **`SIGKILL` after 2 s,** without asking again: the user has agreed to stop the music.
3. **Status 1** for an orphan left running, as today.
4. **`acquire()` retries** (requirement 1), because `ks-play` now probes locks.
5. **Stop late** (requirement 11), so a mistyped `--worker` or `--engine` can't end the music.
