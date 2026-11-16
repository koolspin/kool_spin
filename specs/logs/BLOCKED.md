# Blocked: logs, task 8 (cleanup and handoff)

Written by build pass 2 (after the second restart), 2026-11-16.

## What I was doing

Task 8, the last one: run the whole check set and confirm that every `[auto]` acceptance test passes, then write `pr-description.md`.

## What stopped me

23 acceptance tests can't pass in the loop's sandbox. The sandbox won't let `kool_spin` bind its Unix socket (`can't open the supervisor socket: Operation not permitted`). Every "Logs: …" supervisor test fails at `tests/acceptance/logs/logs_support.h:369` (`REQUIRE( std::filesystem::exists(socket) )`), before it checks anything about logs. The failing tests are:

- the 7 "Logs: …" tests (a session folder, keeps the newest ten sessions, shared timestamps, standard error gets kool_spin's warnings and errors, the default folder, the engine's last lines outlive it, without a log folder);
- 16 tests from earlier steps: 5 "Supervisor rings: …", 8 "Supervisor: …", and 3 "Supervisor socket: …".

These same 23 failed before tasks 1 to 7 changed anything (see "Learnings" in `plan.md`). So the loop has never seen the supervisor half of this feature (tasks 6 and 7) pass its acceptance tests. "Finish the feature" requires every test to pass, and nothing a pass does inside this sandbox can change that, so retrying would use up two more passes for nothing.

## What I checked

All on Linux, at commit `91c8106` plus this file:

- `debug`: all 208 non-acceptance tests pass. Acceptance: 241 of 264 pass. The 23 failures are the tests listed above, and they fail only at the socket.
- `asan`: all 208 non-acceptance tests pass.
- `rtsan`: all 210 non-acceptance tests pass.
- `tsan`: all 208 non-acceptance tests pass.
- `tools/lint.sh`: OK.
- Not run in this pass: the acceptance tests under `asan`, `rtsan`, and `tsan`, and `ks_log_record_fuzz` (task 2 ran it for 1,000,000 inputs, and no code it covers has changed since).

## What I need from the maintainer

One of these:

1. **Run the suite outside the sandbox:** `cmake --workflow --preset debug`, `asan`, `rtsan` (and `tsan` if you want it). If all 264 acceptance tests pass, delete this file and let one more pass (or yourself) write `pr-description.md`. If any "Logs: …" test fails for a reason other than the socket, add a fix task to `plan.md` before task 8 and delete this file.
2. **Or let the loop bind Unix sockets** in its sandbox (a change to the loop's settings, which are protected), then delete this file and rerun the loop. Task 8 can then do everything itself.
