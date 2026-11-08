# kool_spin: notes for AI agents

Open-source DJ application, GPL-3.0-or-later. C++23, CMake, vcpkg, Qt 6 Quick, miniaudio.

- Decisions: `docs/adr/`. Read ADR 0001 before adding a dependency.
- Plan and exit criteria: `docs/roadmap.md`.

## Status

Pre-code: Phase 0 (build scaffold) has not started. Update this section, and add build and test commands, as phases land.

## Real-time rules (non-negotiable)

Code reachable from the audio callback must not:

- Allocate or free memory: no `new`/`delete`, `malloc`, container growth, `std::string` building, allocating `std::function`, or releasing the last reference of a `shared_ptr`.
- Lock a mutex, wait on a condition variable, or call anything that might, including logging.
- Do I/O or make system calls: files, network, console, sleep.
- Throw exceptions.
- Call into Qt, SQLite, FFmpeg, or the JS engine.

Mark audio-thread entry points and every function they call `KS_NONBLOCKING` (it expands to `[[clang::nonblocking]]`). The `rtsan` preset must stay clean. Never suppress a RealtimeSanitizer or function-effect diagnostic without the maintainer's approval.

Talk to the audio thread through lock-free SPSC queues (commands in) and atomics or seqlocks (state out). Objects the audio thread lets go of are handed to a deferred-deletion queue that a non-real-time thread drains.

## Working conventions

- The engine library must not depend on Qt and must be able to render offline, without an audio device, so it can be tested headless.
- Every engine change comes with an offline-render test.
- Add dependencies only through `vcpkg.json` (Qt excepted), only when the current phase needs them, and record them in the ADR.
- Keep diffs small and reviewable: a human reviews every change.
