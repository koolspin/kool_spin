# ks-play and the client library (Phase 1b, step 6)

## Summary

This branch adds `ks::client` (`src/client/`), a library a client process uses to talk to `kool_spin`. With it, a client can connect, make requests, receive `DecodeEnded` notifications, open a command ring, send commands and wait for the engine to apply them, and read a validated snapshot of the engine's state. It also adds `ks-play` (`src/apps/play/`), a command-line client built on that library. Steps 6a and 6b, built interactively before the loop, added the `UnloadDeck`, `DeckUnloaded`, and `DecodeEnded` messages and their handling in the engine and supervisor. The build loop built requirements 12 to 33 in ten tasks, and this pass did the finishing.

## Checks run in the loop's sandbox

- `debug`, `asan`, `rtsan`: every non-acceptance test passes (247 in `debug`/`asan`, 249 in `rtsan`).
- Acceptance tests: 294 of 347 pass in every preset. The other 53 all fail before they check anything, because the sandbox blocks Unix sockets: `kool_spin: can't open the supervisor socket: Operation not permitted`, `REQUIRE( std::filesystem::exists(socket) )`, or `leave_socket_file()` failing. **The maintainer needs to run them outside the sandbox** (`cmake --workflow --preset debug|asan|rtsan`). They are:
  - Supervisor rings (5), Supervisor unload (5), Supervisor tracks (10, all except "usage"), Supervisor (8), Supervisor socket (3), Logs (7), from earlier steps.
  - New in this feature: Session: a ring and a command, connecting, rejected commands, requests and notifications, the snapshot. ks-play: --wait reports a failed decode, errors from kool_spin, every control command, interrupted, load/play/pause, no kool_spin, rejections, status, unload/ping/stop, watching.
- `ks_play_args_fuzz` under `fuzz`: passes (about 60 s).
- `tools/lint.sh`: clean.
- macOS rules checked over `src/client` and `src/apps/play` by reading the code. There's no floating-point `std::from_chars` (reals go through a syntax check, then `std::strtod`), no `std::jthread`, and no `std::min`/`std::max` that mixes `std::uint64_t` with `std::size_t`. Only the `time_point` `std::min` in `session.cpp` remains, with both arguments of one type. There's no `sigtimedwait`/`ppoll`, and `std::format` only formats integers and strings. Only CI on macOS confirms this.

## What was built, task by task

1. **Command builders and rejection names** (`src/client/command.cpp`, `rejection_name()`), with `tests/client/command_test.cpp`.
2. **`Session::snapshot_from()`** (`src/client/snapshot.cpp`) reads status, the four decks, and the mixer through the seqlocks, under one shared deadline. It validates `output_state`, each channel's `side`, and each client's `last_rejection`. Errors read `unstable seqlock: <block>` or `invalid <field>: <value>`.
3. **`connect()`, `connect_as()`, `snapshot()`, `close()`**: `Session::Impl` holds a non-blocking socket. Its `send()` and `receive_until()` never block past a deadline. Once a session is closed or has failed, every later call returns the same error.
4. **Requests and notifications**: `request<Reply>()`, plus `ping`, `load_file`, `unload_deck`, `stop`, and `next_decode_ended`. Notifications wait in `DecodeQueue` (`src/client/decode_queue.h`), which holds at most 64 and drops the oldest.
5. **`open_ring()`, `send()`, `wait_applied()`**: `open_ring()` retries `Unavailable`, maps the ring segment, and waits for the slot to show as attached. `wait_applied()` polls the slot's `ClientAck` through the seqlock.
6. **Numbers and global options** (`src/apps/play/numbers.*`, `options.cpp`): strict parsers, `--runtime-dir`, `--name`, `--help`, and the `load`, `unload`, `status`, `stop`, and `ping` commands.
7. **Control commands**: every command in requirement 22's table.
8. **`main_entry()` and the one-shot commands** (`src/apps/play/run.cpp`): exit statuses 0, 1, 2, and 3, with error lines starting `ks-play: `.
9. **`status` and `status --watch`** (`src/apps/play/status_format.*`): requirement 25's lines, with `%.6g` for reals. `--watch` pings each interval to notice when `kool_spin` has gone.
10. **Signals**: no handler is installed. SIGINT, SIGTERM, and SIGUSR1 are blocked, the work runs on a `std::thread`, and the calling thread `sigwait()`s. When the worker finishes, it wakes the caller with SIGUSR1. Outside `--watch`, a signal exits with `_Exit(130/143)`. In `--watch`, it stops the loop and `ks-play` exits 0. SIGPIPE is ignored while it runs.
11. **Finishing**: this pass ran the checks above and wrote this description. No code changed.

## Deviations and choices

The plan's notes record each choice. These are the ones worth a reviewer's attention:

- **Waking the waiting thread**: the worker sends SIGUSR1, not SIGTERM, because clang-tidy's `bugprone-bad-signal-to-kill-thread` refuses `pthread_kill(…, SIGTERM)`. The plan offered this as one of its two options.
- **`open_ring()`** waits for the slot to show both attached and `commands_read == 0`, so it never mistakes a stale view of the slot's previous occupant for the new ring.
- **`load --wait`** has no overall limit. It polls `next_decode_ended()` every second and skips notifications for other track IDs.
- **Parsing**:
  - Options are taken only before the command word.
  - `--wait` is taken only right after `load`'s path.
  - A real that overflows to an infinity (`1e999`) is refused. Only the word `-inf` means negative infinity, and only where it's allowed.
  - `assign` and `channel-cue` words are lower case and exact.
- **`status`** uses `std::snprintf` with a NOLINT that gives the reason (requirement 25's `%.6g`), because `std::format`'s floating point is avoided for macOS.
- **No unit tests** cover `Session`'s request, ring, and wait paths. A `Session` can't be built without a listener, and the sandbox blocks listeners, so the acceptance tests cover those paths.

## Learnings worth adding to `CLAUDE.md`

- `Status` section (protected): step 6, `ks-play` and `ks::client`, is done, and next is step 7 of Phase 1b (see `docs/roadmap.md`).
- In the loop's sandbox, clang-analyzer flags Catch2's `CHECK_FALSE`/`REQUIRE_FALSE` (`optin.core.EnumCastOutOfRange`), so tests write `CHECK(!x)`.
- clang-tidy's `bugprone-bad-signal-to-kill-thread` refuses `pthread_kill` with SIGTERM. Use a private signal such as SIGUSR1 to wake a thread waiting in `sigwait()`.
- In this environment, `ctest -R` with a test name containing parentheses or colons didn't match. `ctest -I <from>,<to>`, with numbers from `ctest -N`, works.

## `[manual]` acceptance criteria

- [ ] Run `kool_spin` with the real backend on the Inpulse 500. With `ks-play`, load two tracks, play them, move the crossfader, and watch `ks-play status --watch` while the audio plays, with no underruns caused by `ks-play` running.
