# Logs: per-process log files (Phase 1b, step 4a)

## Summary

Each process now logs through a ring in a log segment that the supervisor creates (ADR 0003 §13, `specs/logs/spec.md`). `ks-engine` writes its lines into the segment it gets as descriptor 5 (`--log-fd 5`). `kool_spin` reads that ring at least every 100 ms, and once more after the engine has exited. It writes the engine's lines to `ks-engine.log` and its own to `kool_spin.log`, both in one session folder: `YYYY-MM-DD_HH-MM-SS_<pid>`, under `~/.local/state/kool_spin/logs` (`$XDG_STATE_HOME`) on Linux and `~/Library/Logs/kool_spin` on macOS, or under `--log-dir`. It keeps the newest ten sessions and up to 16 MiB per file. Every line shows the local date and time, converted from the shared steady clock with one offset for the whole session. Standard error gets only `kool_spin`'s warnings and errors. Without a log folder, `kool_spin` runs on, and every line goes to standard error.

All 264 acceptance tests pass, and so do the 33 new unit tests. On Linux, all six workflows passed: `debug`, `asan`, `tsan` and `release` (472 tests each), `rtsan` (474), and `fuzz` (477, with `ks_log_record_fuzz` reading 1,000,000 inputs). `tools/lint.sh` is clean. CI passes the same on Linux and on macOS arm64 (`debug`, `release`, `asan`, `tsan`: 472 of 472 each), as of 175130d. The cable pull was checked by hand on Linux and on macOS.

## What was built, task by task

1. **Writer and reader** (f261b35):
   - **`LogWriter`:** takes a mutex, stamps each line with the steady clock under the mutex so the ring stays in time order, and cuts a line at a newline, or at the last UTF-8 character boundary that fits. When the ring is full, it drops the line and counts it. The count is kept in private memory and only ever written to the segment.
   - **`LogReader`:** copies records out. `take_dropped()` returns the whole count on its first call, and a count that goes down makes the reader corrupt.
2. **Clock and line formats** (632e912), in a new `src/ipc/log_format.cpp`:
   - **`LogClock::now()`** looks up the time zone once per session.
   - **The conversion** works in seconds and nanoseconds, so no clock values can make it overflow.
   - **Dates** come from `std::chrono::year_month_day`.
   - **The UTF-8 decoder** refuses overlong forms, surrogates and values above U+10FFFF. Control characters and invalid bytes are escaped as `\xNN`.
3. **`LogFile`** (9517d06): `O_CREAT|O_EXCL|O_APPEND|O_CLOEXEC`, mode 0600, one `write()` per line. When a line would pass the limit, a single limit warning goes in its place, and every later line is dropped. After a write error, `error()` returns it and nothing more is written.
4. **The engine logs through its segment** (675eca7):
   - **`--log-fd`:** checked and made close-on-exec in the first starting step, then mapped before the `engine` lock.
   - **Where reports go:** with `--log-fd`, every report goes to the log. A starting failure also goes to standard error.
   - **Output lines:** the control thread logs the output's state changes as `output playing on a device at <rate> Hz, <n> channels` and `output freewheeling: no device opened` or `…: the device was lost`. For these, `OutputController` gained `device_sample_rate()` and `device_channels()`.
   - **Refusals:** a refused `Attach` or `Detach` is a warning.
5. **Log folder helpers** (2e4b543), in a new `src/apps/supervisor/log_dir.{h,cpp}`:
   - the default folder;
   - `create_log_dir()`, mode 0700;
   - session names, built and parsed;
   - `remove_old_sessions()`. It works through descriptors opened without following links, and deletes only regular `*.log` files, then the folder once it's empty.
6. **The supervisor opens its logs** (a8d332a):
   - **Clock, folders and files:** after the `supervisor` lock, it reads the session clock, creates the log folder, removes old sessions, then makes the session folder and `kool_spin.log`.
   - **The engine's segment:** it creates the engine's log segment and passes it as descriptor 5.
   - **Levels:** every report now has a level, following the spec's table.
7. **The supervisor reads the engine's ring** (91c8106):
   - **When:** at least every `kLogDrainInterval`, after the engine is reaped, and after it's killed.
   - **How much:** at most one ring's worth per pass, so a busy engine can't hold up the poll loop.
   - **Problems:** dropped lines and a corrupt ring are reported. A log file that fails is reported once, and its lines then go to standard error.
8. **Cleanup and handoff:** finished outside the loop (see "Changed after the loop").

## Deviations and choices

No requirement was changed. The plan's "Learnings" records each choice where the spec left room. The ones you'll see most:

- **Standard error gets less than before.** With a log file, `kool_spin`'s `info` lines, such as `started ks-engine`, `client '<name>' said Hello` and `stopping`, go only to `kool_spin.log`. This follows requirement 15 and the choice made when the step was planned: "kool_spin's warnings and errors" go to standard error, "everything else … only to the files". The spec's wording said otherwise in two places, and has been amended to match (see "Changed after the loop"). No earlier test reads `kool_spin`'s standard error.
- **Wording:**
  - Two report lines each became one: `stopping: got signal <n>`, `stopping: client '<name>' asked` and `stopping: it couldn't start` for `kool_spin`, and `stopping: got a signal` and `stopping: the supervisor asked` for `ks-engine`.
  - `dropped client '<name>': it closed its connection` is `info`. Every other drop is a warning.
- **Which time has to fit:** a line's local time, the one shown, must fall between the years 1970 and 9999. Otherwise it's written as `steady:<n>`, and a header shows `session started steady:<n>`.
- **A limit warning** carries the time of the line it replaces, so a file's times stay in order.
- **`getenv()`** is read once, as `kool_spin` starts, for `HOME` and `XDG_STATE_HOME`, with a `NOLINTNEXTLINE(concurrency-mt-unsafe)` comment. `kool_spin` has no threads.

## Changed after the loop

- **Task 8 was finished interactively.** On Linux, Claude Code's sandbox blocks Unix sockets with a seccomp filter, so in the loop `kool_spin` couldn't open `supervisor.sock`. Inside the loop, the 7 "Logs: …" tests that start `kool_spin` and 16 tests from earlier steps failed at that point, before checking anything about logs. The loop correctly blocked rather than retrying. The full check set above was run outside the sandbox, the code was reviewed, and this description was written then. No code changed.
- **The spec's wording on standard error was amended,** with the maintainer's approval. Before, the "This spec extends" paragraph said the earlier specs' lines to standard error still applied "unchanged", and decision 3 said "besides the lines the earlier specs already send there". Both contradicted requirement 15 and the planning choice. They now say that, with a log file, `kool_spin`'s `info` lines and the engine's reports go only to the files. The same paragraph's reference to "requirements 15 and 16" now says 15 and 18. No code or test changed.
- **A RealtimeSanitizer report on CI, fixed.** Linux `rtsan` failed "Supervisor: an engine that doesn't start" once. When `ks-engine` exited, the `SIGCHLD` handler's `write()` to the self-pipe ran while `kool_spin` was inside `load_shared()`, called from the engine-log reader. That function is `KS_NONBLOCKING`, so RealtimeSanitizer counted the handler as real-time code. It reproduced locally in 20 of 300 runs. `kool_spin` now blocks the signals it handles while it reads the engine's ring, then restores its mask (`SignalsBlocked` in `run.cpp`). A signal that arrives during the read is handled just after it. Nothing is suppressed. After the fix, 2,000 runs had no failures.
- **Two usage-limit stops.** Build passes that hit the account's usage limit end at once and make no commit, so the runner counted three of them as "no progress" and wrote `BLOCKED.md` twice (ce8014e and 1499356). The maintainer deleted it each time and resumed.

## Learnings worth adding to `CLAUDE.md`

- **Supervisor tests can't run in a build loop:** on Linux, Claude Code's sandbox blocks Unix sockets, so tests that start `kool_spin` fail there. Allowing Unix sockets in the loop's sandbox would also expose the session bus and the SSH agent, so it needs a decision before the next loop-built step that uses the socket.
- **Signals and `KS_NONBLOCKING` code:** under RealtimeSanitizer, a signal handler that interrupts a `KS_NONBLOCKING` function runs as real-time code, even in a process that isn't real-time. A process that handles signals with a self-pipe must block them while it calls the shared-memory primitives. `ks-engine` has the same pattern: its control thread calls `KS_NONBLOCKING` code, including `LogWriter::write()` now, while its handlers can run. It hasn't failed yet, but it should get the same treatment, together with keeping these signals off the audio thread, in step 4b.
- **Sandbox placeholder files:** while a pass runs, the sandbox leaves empty, read-only placeholders such as `.bashrc` and `.gitconfig` at the top of the worktree. Stage files by name, never with `git add -A`.
- **Phase 1b status:** step 4a is done. The Status section is the maintainer's to update.

## To check by hand

- [x] `[manual]` **Device events:** with `kool_spin` playing through the Inpulse 500, pull its USB cable and plug it back in. `ks-engine.log` should get a `warning` line about freewheeling and then an `info` line about playing, with the device's rate and channels, at the times it happened. Check on Linux and on macOS, in the default log folder. Checked by the maintainer on both, 2026-11-16.
- [x] **CI on macOS arm64** (`debug`, `release`, `asan`, `tsan`): build loops build only on Linux. All four pass, 472 of 472 each.
- [x] **Protected paths:** the spec, including the standard-error amendment and the device-events tick; `tests/acceptance/logs/`; and the `--log-dir` changes to the processes and client-rings test helpers, which the spec approves (decision 8). They need the `protected-paths-approved` label.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
