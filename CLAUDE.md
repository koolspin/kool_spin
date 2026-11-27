# kool_spin: notes for AI agents

Open-source DJ application, GPL-3.0-or-later. C++23, CMake, vcpkg, Qt 6 Quick, miniaudio. It runs as several processes so the music keeps playing when any part fails.

- Decisions: `docs/adr/`. ADR 0001 covers the stack; ADR 0002 covers reliability and the process model; ADR 0003 covers engine threading, IPC, and memory. Read them before adding a dependency, crossing a process boundary, or changing the audio thread or shared memory.
- Plan and exit criteria: `docs/roadmap.md`.
- How features get built (specs, acceptance tests, build loops): `docs/process.md`.

## Protected paths

`tools/loop/protected-paths` lists files that define "done", the checks themselves, and decisions the maintainer owns: acceptance and guardrail tests, build and lint configuration, `vcpkg.json`, this file, the ADRs, approved specs, the loop machinery, and CI. Build loops can't edit them. Any pull request that changes one needs the maintainer's `protected-paths-approved` label. In an interactive session, change them only when the maintainer asks.

## Status

Phase 0 is complete (2026-11-09). It delivered the build, the checks and guardrails, CI on Linux and macOS, and the build-loop workflow, proven on its first feature (`ks-tone` options).

Phase 1, the headless engine and process model, is split in two by ADR 0003. Phase 1a, the engine as a library in one process, is complete (2026-11-14). It delivered the shared-memory primitives, the render core and its drivers with device loss, the output safety stage, the deck, the mixer, commands in and state out, the resume state, miniaudio devices, and the development harness (`ks-harness`), soaked for an hour and cable-pulled on the Inpulse 500.

Phase 1b, separate processes (supervisor, engine, decode worker, and the `ks-play` client) and crash recovery, is under way; see `docs/roadmap.md`. Steps 1 to 6, 7a, 7b, 8, and 9a to 9c are done: the shared-memory segments, the socket protocol, `kool_spin` starting `ks-engine` and welcoming clients, the clients' command rings, per-process logs, the audio thread's events in the engine's log, the decode worker, loading tracks through the supervisor into the engine, and `ks-play`, the command-line client (with `ks::client`, unloading decks, and decode results); 7a, restarting the engine when it ends, with a crash-loop guard and a recording segment for chaos tests; 7b, hang detection (the supervisor kills an engine whose heartbeat stops for 500 ms, and reports one whose control thread stops answering pings); 8, the supervisor crash (the engine plays on, clients keep their rings, and a relaunch offers to stop the engine, with a prompt or `--stop-previous`); 9a, the state folder (a second, best-effort lock for the supervisor and the engine, so that a relaunch finds an orphan whose runtime directory changed); 9b, writing the session journal (the supervisor's record of the decks and the mixer, in the state folder, kept up to date atomically by a writer thread, and fuzzed); and 9c, restoring the session at launch, paused (entries that can't be restored are carried to the next launch). Next is step 10, health checks and sleep blocking. Update this section as steps land.

## Build and test

Needs `VCPKG_ROOT` set to a vcpkg checkout. On Linux, Clang 21 is picked automatically (`cmake/KsToolchain.cmake`).

- Configure, build, and test: `cmake --workflow --preset <preset>`. Presets: `debug`, `release`, `asan`, `tsan`, `rtsan`, `fuzz` (the last two Linux only).
- Step by step: `cmake --preset debug`, `cmake --build --preset debug`, `ctest --preset debug`.
- Formatting and clang-tidy: `tools/lint.sh` (reads `build/debug/compile_commands.json`, so configure `debug` first). `tools/lint.sh --fix` applies formatting fixes.

Before committing: the `debug`, `asan`, and `rtsan` workflows pass, and `tools/lint.sh` is clean. Warnings are errors. On a feature branch whose acceptance tests aren't all passing yet, run `ctest --preset <preset> -LE acceptance` instead of the full workflow (see `docs/process.md`).

In a build loop on Linux, Claude Code's sandbox blocks Unix sockets, so every test that starts `kool_spin` fails there, before it checks anything: `can't open the supervisor socket: Operation not permitted`. That isn't the loop's to fix. Write `BLOCKED.md` saying so rather than retrying; the maintainer runs those tests outside the sandbox.

CI (`.github/workflows/ci.yml`) runs every preset on Linux, `debug`/`release`/`asan`/`tsan` on macOS arm64, and lint, on every pull request and on pushes to `main`. The LLVM version is pinned in three places that must agree: `cmake/KsToolchain.cmake`, `tools/lint.sh`, and `ci.yml`. vcpkg is pinned by `builtin-baseline` in `vcpkg.json`; CI checks out exactly that commit.

macOS `tsan` doesn't report data races in struct copies, by assignment or `memcpy`, only in scalar accesses, so Linux `tsan` is the check for data races.

## Code conventions

- Types, enumerators, and template parameters are `CamelCase`. Functions, variables, and namespaces are `snake_case`. Private and protected members end in `_`. Constants are `kCamelCase`. Macros are `KS_UPPER_CASE`. clang-tidy enforces all of these.
- clang-tidy rejects indexing an array (`std::array` or built-in) with an index that isn't a constant expression. Index through a span instead (`std::span{array}[i]`), or loop with a range-for.
- Everything lives in namespace `ks`. Public headers go in `src/<lib>/include/ks/<lib>/` and are included as `<ks/<lib>/<name>.h>`.
- Out-of-line definitions repeat `noexcept KS_NONBLOCKING`: the attribute is part of the function's type, and the build fails if a definition doesn't match its declaration.
- Executables live in `src/apps/<app>/`, split into a library (everything except argument handling, so tests can use it) and a thin `main.cpp`. `ks-tone` is a standalone diagnostic tool, outside the process model.
- `third_party/` compiles third-party code that vcpkg ships as headers only (such as miniaudio). It isn't linted and doesn't use `ks::warnings`.
- Code must also compile on macOS with AppleClang 17 and its libc++. Build loops only build on Linux, so macOS problems surface only in CI. Known gaps in that libc++: no floating-point `std::from_chars` (parse with a strict syntax check, then `std::strtod`; integer `std::from_chars` is fine), and no `std::jthread` (use a `std::thread` and join it). The 64-bit integer types differ too: on Linux, `std::uint64_t` and `std::size_t` are both `unsigned long`, but on macOS `std::uint64_t` is `unsigned long long`. A template that deduces one type from both, such as `std::min` or `std::max`, doesn't compile there, so name the type (`std::min<std::uint64_t>(a, b)`).
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
- Trust data read from shared memory: other processes write it. Copy a value into private memory, validate it (indices, lengths, enum values, ranges), and use only the copy; never read a shared field twice expecting the same value. Take a segment's size from the engine's own mapping, never from a header another process wrote. Write a range check on a real as `x >= lo && x <= hi`, so that NaN fails it; a negated comparison such as `!(x < lo)` lets NaN through.

Mark audio-thread entry points and every function they call `noexcept KS_NONBLOCKING`, from `<ks/core/realtime.h>` (it expands to `[[clang::nonblocking]]`; the build fails if such a function allocates, blocks, or calls an unmarked function). The `rtsan` preset must stay clean. Never suppress a RealtimeSanitizer or function-effect diagnostic without the maintainer's approval.

Under RealtimeSanitizer, a signal handler that interrupts a `KS_NONBLOCKING` function runs as real-time code, even in a process with no audio thread, so its `write()` to a self-pipe is reported. A process that handles signals blocks them with `pthread_sigmask` while it calls `KS_NONBLOCKING` code, such as the shared-memory readers (`SignalsBlocked` in `src/apps/supervisor/run.cpp`), and keeps them blocked on the audio thread. `ks-engine` installs no handlers at all: before any thread starts, it blocks its stop signals, so every thread inherits the mask, and its control thread takes them with `sigpending()` and `sigwait()`.

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
- Stage files by name, never with `git add -A` or `git add .`. In a build loop, the sandbox leaves empty, read-only placeholder files, such as `.bashrc`, `.gitconfig`, and `.mcp.json`, at the top of the worktree.
