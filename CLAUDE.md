# kool_spin: notes for AI agents

Open-source DJ application, GPL-3.0-or-later. C++23, CMake, vcpkg, Qt 6 Quick, miniaudio. It runs as several processes so the music keeps playing when any part fails.

- Decisions: `docs/adr/`. ADR 0001 covers the stack; ADR 0002 covers reliability and the process model; ADR 0003 covers engine threading, IPC, and memory. Read them before adding a dependency, crossing a process boundary, or changing the audio thread or shared memory.
- Plan and exit criteria: `docs/roadmap.md`.
- How features get built (specs, acceptance tests, build loops): `docs/process.md`.

## Protected paths

`tools/loop/protected-paths` lists files that define "done", the checks themselves, and decisions the maintainer owns: acceptance and guardrail tests, build and lint configuration, `vcpkg.json`, this file, the ADRs, approved specs, the loop machinery, and CI. Build loops can't edit them. Any pull request that changes one needs the maintainer's `protected-paths-approved` label. In an interactive session, change them only when the maintainer asks.

## Status

Phase 0 is complete (2026-11-09). It delivered the build, the checks and guardrails, CI on Linux and macOS, and the build-loop workflow, proven on its first feature (`ks-tone` options). Still open: listening to `ks-tone` through JACK and on macOS.

Next is Phase 1, the headless engine and process model. ADR 0003 splits it in two: Phase 1a builds the engine as a library in one process, and Phase 1b separates the processes and adds crash recovery. Phase 1a starts interactively with the shared-memory primitives; see `docs/roadmap.md`. Update this section as phases land.

## Build and test

Needs `VCPKG_ROOT` set to a vcpkg checkout. On Linux, Clang 21 is picked automatically (`cmake/KsToolchain.cmake`).

- Configure, build, and test: `cmake --workflow --preset <preset>`. Presets: `debug`, `release`, `asan`, `tsan`, `rtsan`, `fuzz` (the last two Linux only).
- Step by step: `cmake --preset debug`, `cmake --build --preset debug`, `ctest --preset debug`.
- Formatting and clang-tidy: `tools/lint.sh` (reads `build/debug/compile_commands.json`, so configure `debug` first). `tools/lint.sh --fix` applies formatting fixes.

Before committing: the `debug`, `asan`, and `rtsan` workflows pass, and `tools/lint.sh` is clean. Warnings are errors. On a feature branch whose acceptance tests aren't all passing yet, run `ctest --preset <preset> -LE acceptance` instead of the full workflow (see `docs/process.md`).

CI (`.github/workflows/ci.yml`) runs every preset on Linux, `debug`/`release`/`asan`/`tsan` on macOS arm64, and lint, on every pull request and on pushes to `main`. The LLVM version is pinned in three places that must agree: `cmake/KsToolchain.cmake`, `tools/lint.sh`, and `ci.yml`. vcpkg is pinned by `builtin-baseline` in `vcpkg.json`; CI checks out exactly that commit.

macOS `tsan` doesn't report data races in struct copies, by assignment or `memcpy`, only in scalar accesses, so Linux `tsan` is the check for data races.

## Code conventions

- Types, enumerators, and template parameters are `CamelCase`. Functions, variables, and namespaces are `snake_case`. Private and protected members end in `_`. Constants are `kCamelCase`. Macros are `KS_UPPER_CASE`. clang-tidy enforces all of these.
- Everything lives in namespace `ks`. Public headers go in `src/<lib>/include/ks/<lib>/` and are included as `<ks/<lib>/<name>.h>`.
- Out-of-line definitions repeat `noexcept KS_NONBLOCKING`: the attribute is part of the function's type, and the build fails if a definition doesn't match its declaration.
- Executables live in `src/apps/<app>/`, split into a library (everything except argument handling, so tests can use it) and a thin `main.cpp`. `ks-tone` is a standalone diagnostic tool, outside the process model.
- `third_party/` compiles third-party code that vcpkg ships as headers only (such as miniaudio). It isn't linted and doesn't use `ks::warnings`.
- Code must also compile on macOS with AppleClang 17 and its libc++. Build loops only build on Linux, so macOS problems surface only in CI. Known gaps in that libc++: no floating-point `std::from_chars` (parse with a strict syntax check, then `std::strtod`; integer `std::from_chars` is fine), and no `std::jthread` (use a `std::thread` and join it).
- Every source file starts with `// SPDX-License-Identifier: GPL-3.0-or-later`.
- Tests: `<name>_test.cpp`, registered with `ks_add_test()` in `tests/CMakeLists.txt`.
- `tests/guardrails/` holds tests proving that the checks themselves work (for example, that RealtimeSanitizer reports an allocation in a `KS_NONBLOCKING` function). Never weaken, skip, or delete them.

## Processes

| Process | Role | Allowed dependencies |
|---|---|---|
| Supervisor | Starts and restarts the others; owns shared memory; writes the session journal | Standard library only |
| Engine | Audio I/O, decks, mixer, output safety stage, MIDI, controller scripts | miniaudio, libremidi, QuickJS-ng |
| UI | Qt Quick interface; hosts the library database | Qt, SQLite |
| Workers | Decoding, tags, analysis | FFmpeg, TagLib, qm-dsp, libebur128 |

- Never add a dependency to the engine or supervisor without updating ADR 0002.
- Never put a process boundary inside the per-buffer audio path.
- Never make the engine wait on another process.
- The supervisor is the hub: every socket connects to it, and it creates every shared-memory segment and passes it on as a file descriptor. The engine's only socket is to the supervisor (ADR 0003).

## Real-time rules (non-negotiable)

Code reachable from the audio callback must not:

- Allocate or free memory: no `new`/`delete`, `malloc`, container growth, `std::string` building, allocating `std::function`, or releasing the last reference of a `shared_ptr`.
- Lock a mutex, wait on a condition variable, or call anything that might, including logging.
- Do I/O or make system calls: files, network, console, sleep.
- Throw exceptions.
- Call into Qt, SQLite, FFmpeg, or the JS engine.
- Trust data read from shared memory: other processes write it. Copy a value into private memory, validate it (indices, lengths, enum values), and use only the copy; never read a shared field twice expecting the same value. Take a segment's size from the engine's own mapping, never from a header another process wrote.

Mark audio-thread entry points and every function they call `noexcept KS_NONBLOCKING`, from `<ks/core/realtime.h>` (it expands to `[[clang::nonblocking]]`; the build fails if such a function allocates, blocks, or calls an unmarked function). The `rtsan` preset must stay clean. Never suppress a RealtimeSanitizer or function-effect diagnostic without the maintainer's approval.

Talk to the audio thread through lock-free SPSC rings (commands in) and seqlocks (state out), both in shared memory. Commands carry absolute values ("rate 1.02", not "rate up"), so replaying one is harmless. Objects reach the audio thread through a give ring and leave through a retire ring, which the engine's control thread drains and frees. ADR 0003 has the details, including the memory ordering the seqlock needs.

## Reliability rules

- The output safety stage (NaN/Inf to silence, limiter, denormal flushing, gain ramps) is always the last stage before the device. Never bypass it.
- The engine never reads files during playback; tracks are fully decoded into memory first.
- Never modify users' music files unless they have opted in.
- Every failure path gets an automated test: chaos tests for crashes and device loss, fuzzing for anything that parses outside input.

## Working conventions

- The engine library must not depend on Qt and must be able to render offline, without an audio device, so it can be tested headless.
- Every engine change comes with an offline-render test.
- Engine output must not depend on how frames are split into blocks: smoothing, gain ramps, and the limiter work per sample and carry their state across blocks. Golden files are compared with a tolerance; block-size tests are bit-exact (ADR 0003).
- Add dependencies only through `vcpkg.json` (Qt excepted), only when the current phase needs them, and record them in the ADRs.
- Keep diffs small and reviewable: a human reviews every change.
