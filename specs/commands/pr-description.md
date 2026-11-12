# Commands in (Phase 1a)

## Summary

The engine now takes commands. Clients write 64-byte command slots to SPSC rings. At the start of each block, `Engine::process()` reads up to 32 slots from each attached ring, in slot order. It decodes and validates each slot with `decode()` and applies it to the mixer with `apply()`. Tracks reach the audio thread through a give ring. Every reference, and every detached client ring, comes back exactly once through a retire ring, and only after the deck has let go of it. Everything reachable from `process()` is `KS_NONBLOCKING`, and nothing on the audio thread allocates or waits.

Every check passes on Linux: `cmake --workflow --preset` `debug`, `asan`, `rtsan`, `tsan`, and `fuzz`, with acceptance tests included (220 tests; 222 under `rtsan`, 221 under `fuzz`), and `tools/lint.sh`.

## What was built, task by task

1. **The decoder** (`src/engine/command.cpp`, `decode()`). It checks the type as the full 32-bit field against 1..14 before narrowing, so 0x101 is `UnknownType`. Then it checks the target, then padding, then values. NaN and ±∞ are found from the bit pattern. −∞ is allowed only for trim and the two levels. Flags must be 0 or 1, sides 0 to 2, and bus maps two different even channels below `kMaxOutputChannels`, compared as 64-bit words before narrowing. Only the fields a type uses are filled in, so encoding a decoded `Command` gives the same 64 bytes back (−0.0 included). Unit tests: `tests/engine/command_test.cpp`.
2. **`apply()`**: a `switch` that calls the matching mixer or deck control. For a deck command, a deck index of `kDecks` or more changes nothing. Unit tests compare `apply()` with the direct call: settings, deck state, and one rendered block, bit for bit.
3. **Clients and the give and retire rings** (`engine.h` private members, `engine.cpp`). `attach()`, `detach()`, and `collect()` work on the control thread. The audio thread applies handovers, reads commands fairly with per-client statistics, and marks a corrupt ring's client as corrupt. Invalid blocks (0 frames, more than `kMaxBlockFrames`, or a sample rate of 0) do nothing at all. Releases wait in a fixed-capacity FIFO (`kDecks * 4 + kMaxClients` entries) when the retire ring is full. Unit tests: `tests/engine/engine_test.cpp`.
4. **Loads, unloads, and references.** Each deck has 4 reference slots: free, live, or letting go. A duplicate load of a live track is counted on its slot (`owed`) and needs no new slot. When a load finds no free slot, it waits in `stalled_` and is retried first in the next block, so order is kept. Each block ends with a let-go check (a live reference whose deck no longer holds its track starts letting go), then the release queue drains as far as the retire ring has room.
5. **Concurrency.** No code change was needed. The full `tsan` and `asan` suites pass with no reports. Review confirmed that the control methods touch only `control_clients_`, `give_producer_`, and `retire_consumer_`, so the two rings are the only state both threads share.
6. **Finish.** All five workflows and lint were run, and pass. The stub comments and `NOLINT` blocks were already gone (passes 2 and 4). The private doc comments in `engine.h` were checked, and match the code.

## How each `[auto]` criterion is covered

All of these are tests in `tests/acceptance/commands/`, and they pass in `debug`, `asan`, `rtsan`, and `tsan`:

- Slot layout: "Commands: the slot layout".
- Each type decodes: "Commands: every type decodes".
- Rejections and check order: "Commands: invalid slots are rejected".
- Round trips (1,000,000 slots, every preset): "Commands: decoding and encoding round-trip".
- Commands match direct calls: "Engine: commands do what direct calls do".
- Statistics: "Engine: every command read is counted".
- Fairness: "Engine: rings are read fairly, in slot order".
- Loading: "Engine: loads hand tracks over and release them".
- References: "Engine: every reference is released once, after its deck lets go".
- Back pressure: "Engine: full rings delay, but lose nothing".
- Clients: "Engine: clients attach and detach".
- Block-size invariance: "Engine: output doesn't depend on block size".
- Under load, with ThreadSanitizer clean: "Engine: the control thread and the audio thread share nothing unsafely". It passes under `tsan` with no reports.
- Real-time safety: "Commands are real-time safe". It compiles under function-effect analysis and is clean under `rtsan`.
- Fuzzing: `ks_command_decoder_fuzz` runs 1,000,000 inputs under the `fuzz` preset with no sanitizer report.
- **Every CI configuration:** only CI can check this. The loop builds only on Linux, so the macOS arm64 jobs (`debug`, `release`, `asan`, `tsan` with AppleClang 17 and libc++) are unverified until the pull request's CI runs. Nothing here uses floating-point `std::from_chars` or `std::jthread`.

## Deviations from the spec or plan

- **Task 3's planned "ring release waits while the retire ring is full" unit test** was replaced by a release-order test. It can't be built from rings alone: at most 8 ring releases can be outstanding, and the retire ring holds 64. Task 4's back-pressure unit test and the acceptance test cover a full retire ring.
- **Release order within one block:** when several references let go in the same block, they're released in reference-slot order, not load order. The spec doesn't order them.
- **Back pressure:** a stalled load is held outside the give ring (`stalled_`), because `RingConsumer` has no peek. So 65 handovers can be outstanding while one is stalled. `load()` still returns false once the ring holds 64 (see the plan's Decisions).
- **`apply()` with a deck index `>= kDecks`** (only possible with a hand-built `Command`, since `decode()` rejects it) does nothing, because `Mixer::deck()` requires a valid index.
- No annotation was needed on `RingConsumer`'s constructor. Its effects are inferred, and `spsc_ring.h` is unchanged.

## Decisions (from the plan)

- Check the type as 32 bits before converting to `CommandType`.
- Duplicates of a live reference don't take a slot. A reference that is letting go keeps its slot until its release is pushed, and a later load of that track takes a new slot rather than reviving it.
- A load that has to wait is popped and held in `stalled_`, and is applied before anything else is popped.
- Releases go through a fixed FIFO. Each slot or client is queued at most once.
- A client's consumer is built on the audio thread when its `Attach` is applied.
- An invalid block does nothing, not even handovers.

## Learnings worth adding to `CLAUDE.md`

- clang-tidy flags `opt.value()` and `opt->` in tests even after `REQUIRE(opt.has_value())`. Use `value_or()`.
- clang-tidy rejects indexing a `std::array`/C array with a non-constant index in some contexts (`payload[i]`). `std::span::subspan` keeps the code clean.
- In the loop sandbox, run `ctest` and `cmake --workflow` under `cmake -E env XDG_RUNTIME_DIR=<worktree>/build`, with the path written out (no `$PWD`).

## `[manual]` criteria

None. The spec has no `[manual]` criteria: nothing here is audible that the mixer's listening test didn't already cover.
