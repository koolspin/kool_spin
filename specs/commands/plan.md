# Plan: commands

Written by the planning pass from `spec.md`. Each build pass takes the first unchecked task, ticks it when every check passes, and adds a note beneath it. A failed attempt is recorded as a note; three failed attempts on one task mean the loop writes `BLOCKED.md`.

**Files involved:**
- Edit: `src/engine/command.cpp` and `src/engine/engine.cpp` (the stubs); `src/engine/include/ks/engine/engine.h`, **private members and private types only** (the public API must not change; spec, "The API exists before the code"); `tests/CMakeLists.txt` (not protected: add new unit tests to `ks_engine_tests`).
- New: `tests/engine/command_test.cpp`, `tests/engine/engine_test.cpp` (unit tests, with their own small helpers; don't edit the acceptance helpers).
- Read, don't edit (protected): `tests/acceptance/commands/` (`decoder_test.cpp`, `engine_test.cpp`, `realtime_test.cpp`, `decoder_fuzz.cpp`, `codec_support.h`, `test_support.h`) and `cmake/KsFuzz.cmake` (`ks_add_fuzz_target()` already exists).
- Don't change: `src/ipc/include/ks/ipc/command.h`. It isn't protected, but it's complete, and it's a layout.
- Useful references: `<ks/ipc/spsc_ring.h>` (`SpscRing`, `RingProducer::try_push`, `RingConsumer::try_pop`, `corrupt()`), `Deck::load()/unload()/holds()` in `<ks/engine/deck.h>`, `Mixer` in `<ks/engine/mixer.h>`, `kMaxBlockFrames` (512) in `<ks/engine/block.h>`, and `specs/mixer/plan.md` for how earlier loops worked.

**State before the loop:** the stubs compile. Of the 14 commands acceptance tests, only "Commands: the slot layout" passes (per the commit that added them). The fuzz target builds and runs only under the `fuzz` preset.

## Tasks

- [x] 1. **The decoder.** Implement `decode()` in `command.cpp` (requirements 3 and 4):
  - check `slot.type` as the full `std::uint32_t` against 1..14 **before** casting to `CommandType` (0x101 must be `UnknownType`); then classify the type as deck (1–4), channel (5–8), or global (9–14);
  - `BadTarget`: deck/channel target `>= kDecks`; global target `!= 0`;
  - `NonzeroPadding`: `payload[1..5]` must be 0, except `payload[1]` for `SetBusMap`;
  - `BadValue`: doubles via `std::bit_cast<double>(payload[0])`: reject NaN (any sign or payload) and +∞; reject −∞ except for `SetTrim`, `SetMainLevel`, `SetHeadphoneLevel`; flags exactly 0 or 1; sides exactly 0, 1, 2; bus maps with both channels even, `< kMaxOutputChannels`, and different (payload words are 64-bit: compare before narrowing);
  - build the `Command` with only the fields the type uses (deck for deck/channel commands; others at their defaults), so encoding it back gives the same 64 bytes (−0.0 stays −0.0 because `value` holds the decoded bits).

  Unit tests in a new `tests/engine/command_test.cpp`: one valid slot per type, each rejection reason, the check order (a slot with several faults), the 0x101 type, −0.0 and the largest finite double accepted. (passes: "Commands: every type decodes", "Commands: invalid slots are rejected", "Commands: decoding and encoding round-trip"; under `fuzz`: `ks_command_decoder_fuzz`)

  - Pass 1: `decode()` done (NaN/±∞ checked on the bit pattern as integers; padding loop uses `std::span::subspan` because clang-tidy rejects a non-constant `payload[i]`); 7 unit tests in `tests/engine/command_test.cpp`. 4 of 14 commands acceptance tests pass now (layout, every type, rejections, round-trip). `apply()` is still a stub with a one-line "Stub" comment. The `fuzz` preset wasn't run this pass; task 6 runs it. In this pass, `ctest` worked with `cmake -E env XDG_RUNTIME_DIR=...` and `;`-joined commands, but `for` loops and `$var` were refused.

- [ ] 2. **`apply()`.** In `command.cpp`, a `switch` on `command.type` calling the control requirement 5 names: `SetPlaying` → `mixer.deck(d).set_playing(on)`, `SetRate` → `set_rate`, `Seek` → `seek`, `SetCuePoint` → `set_cue`, `SetChannelCue` → `Mixer::set_cue(d, on)`, `SetCrossfaderAssign` → `set_crossfader_assign(d, side)`, `SetBusMap` → `set_bus_map(bus_map)`, the rest → the mixer control of the same name with `value`. Check the exact signatures in `mixer.h`/`deck.h` (non-const `deck()` accessor). Unit tests: for each type, `apply()` on one mixer and the direct call on another give equal `settings()` and deck state. (advances: "Engine: commands do what direct calls do", "Commands are real-time safe")

- [ ] 3. **Engine: the give and retire rings, clients, and command reading.** In `engine.h` (private) and `engine.cpp`:
  - a private trivially copyable `Handover { Kind kind; std::size_t index; const Track* track; ipc::CommandRing* ring; }` with `Kind` = `Load`, `Unload`, `Attach`, `Detach`; members `SpscRing<Handover, kHandoverCapacity> give_` and `SpscRing<Released, kHandoverCapacity> retire_` (private memory), with a `RingProducer`/`RingConsumer` for each, constructed in the constructor;
  - **control-thread state** (touched only by the control methods): per client slot, the ring pointer (null when free) and whether a detach was requested. `attach()`: lowest free slot, push `Attach`; on success mark it in use. `detach()`: slot `< kMaxClients`, in use, not already detaching; push `Detach`. `collect()`: pop from `retire_`; if it's a ring, free the slot holding that pointer;
  - **audio-thread state:** per client slot, `std::optional<ipc::RingConsumer<ipc::CommandSlot, ipc::kCommandRingCapacity>>`, the ring pointer, and a `ClientStats`;
  - `process()`: return at once if `frames == 0`, `frames > kMaxBlockFrames`, or `sample_rate == 0`. (1) pop handovers: `Attach` emplaces the consumer and resets the stats to `{.attached = true}`; `Detach` resets the consumer, sets `attached = false`, and queues the ring's release; (2) for slots 0..7 with a consumer and not corrupt, pop up to `kCommandsPerBlock`, counting each as requirement 9 says, then `decode()` and `apply()`; if the consumer reports `corrupt()`, set `stats.corrupt` and stop reading it; (3) `mixer_.process(block)`; (4) push queued releases to `retire_` in order, keeping those that don't fit for a later block;
  - the **release queue** is a fixed-capacity FIFO member (no allocation), sized for every reference slot task 4 adds (`kDecks * 4`) plus `kMaxClients` rings; design it now so task 4 only adds entries;
  - `client_stats(i)`: `{}` for `i >= kMaxClients`, else the slot's stats.
  - `load()`/`unload()` stay stubs (return false) in this task.

  Unit tests in a new `tests/engine/engine_test.cpp`: an invalid block does nothing (an attach given before it isn't applied); detach of a free slot fails; a ring release waits while the retire ring is full and none is lost. (passes: "Engine: every command read is counted", "Engine: rings are read fairly, in slot order", "Engine: clients attach and detach")

- [ ] 4. **Engine: loads, unloads, and references.** Requirements 10, 11, and 13:
  - `load(deck, track)`: `deck < kDecks`, push `Load`; `unload(deck)`: push `Unload`;
  - per deck, 4 **reference slots**, each `{const Track* track; std::uint64_t owed; State state}` where `State` is free, live, or letting go (deck no longer holds it, release queued);
  - applying a `Load`: if a **live** slot on that deck already has this track, it's a duplicate: `++owed` (queue the slot if it isn't queued), and call `Deck::load()`. Otherwise take a free slot, mark it live, and call `Deck::load()`; with no free slot, the handover **waits**: keep it in a private `std::optional<Handover> stalled_`, stop applying handovers this block, and retry it first next block (so order is kept). A slot that's letting go isn't a duplicate target: its reference is already committed to release (see Decisions);
  - `Unload`: `Deck::unload()`; references are released through the let-go check below;
  - releases (step 4): first, for each live slot whose deck no longer `holds()` its track, mark it letting go and queue it (this covers refused loads, replaced waiting loads, and finished fades); then drain the queue: for each entry, push its `owed` duplicate releases, then, if it's letting go, its own release and free the slot; ring entries push one release. Stop at the first push that fails and keep the rest, partly drained, for the next block;
  - `load()` returns false when the give ring is full (the ring has 64 slots; a stalled handover sits outside it, see Decisions).

  Unit tests: a duplicate load releases one reference in the same block and keeps the deck's track; a refused load (sample rate 1000) is released in the same block; with `collect()` never called, a 5th distinct reference on a deck stalls the give ring, and everything drains in order once collected. (passes: "Engine: commands do what direct calls do", "Engine: loads hand tracks over and release them", "Engine: every reference is released once, after its deck lets go", "Engine: full rings delay, but lose nothing", "Engine: output doesn't depend on block size", "Commands are real-time safe")

  If this task grows past about 200 lines, split it: 4a references, duplicates, and releases, with the stall implemented but only the simplest unit test; 4b the back-pressure unit tests and any fixes they need.

- [ ] 5. **Concurrency.** Make "Engine: the control thread and the audio thread share nothing unsafely" pass in `debug`, `asan`, and `tsan` (`cmake --workflow --preset tsan`). Check that control-thread state and audio-thread state share nothing except the two rings, and that `client_stats()`/`mixer()` are only read on the audio thread. Fix anything ThreadSanitizer reports; never suppress it. (passes: "Engine: the control thread and the audio thread share nothing unsafely")

- [ ] 6. **Finish.** Run the complete suite with `cmake --workflow --preset` `debug`, `asan`, `rtsan`, and `tsan`, plus the `fuzz` preset (`ks_command_decoder_fuzz`, 1,000,000 inputs), and `tools/lint.sh`. Remove the "Stubs" comments in `command.cpp` and `engine.cpp` and the `NOLINT` block around the former stubs. Check the private doc comments in `engine.h`. Write `specs/commands/pr-description.md`: what changed, how each `[auto]` criterion is covered, the macOS CI criterion (only CI can check it), the Decisions below, and Learnings. (passes: everything the loop can pass)

## Decisions

- **Order.** The decoder first: it's pure, and it passes three acceptance tests (and the fuzz target) on its own. `apply()` next, then the client side of the engine, which needs no tracks and passes three more. Tracks and references last, since every rendering test that plays audio depends on them.
- **Checking the type.** Compare the 32-bit field against 1..14 before converting to `CommandType` (whose base type is `std::uint8_t`), as the commit adding 0x101 to the tests explains.
- **Duplicates don't need room; let-go references do.** The give-ring test loads the same track 64 times in one block and expects none to wait, so a duplicate of a **live** reference is counted on that reference (`owed`) rather than taking a slot. The other back-pressure test alternates two tracks with nothing collected, and expects loads to stall eventually; that only happens if a reference the deck has let go of keeps its slot until its release is pushed, and isn't revived by a later load of the same track. A later load of that track takes a new slot.
- **Waiting handovers.** `RingConsumer` has no peek, so a handover that has to wait is popped and held in `stalled_`, and applied before anything else is popped. This means 65 handovers can be outstanding while one is stalled; the spec's "returns false once 64 handovers are waiting" holds for the ring, and the test only checks the stall-free case.
- **Release order.** A FIFO of release entries, in the order deck slots let go (or duplicates arrive) and rings are detached. Each slot or client is queued at most once, so a fixed array of `kDecks * 4 + kMaxClients` entries is enough.
- **Where the client consumer is built.** On the audio thread, when the `Attach` is applied (`std::optional::emplace`). The tail doesn't move while nobody consumes, so this is the same as "from its tail at `attach()`". If function-effect analysis rejects the `RingConsumer` constructor (it's an inline template, so its effects should be inferred), adding `KS_NONBLOCKING` to its declaration and definition in `spsc_ring.h` is a correct annotation (it only loads atomics); record it here, and don't suppress the diagnostic.
- **Invalid blocks.** Nothing at all happens, not even handovers (requirement 8).

## Learnings

- From earlier loops: in the loop sandbox, run `ctest` as `cmake -E env XDG_RUNTIME_DIR=<worktree>/build ctest --preset <preset> ...` (Catch2 discovery needs a writable runtime dir), pass the worktree path literally (no `$PWD`), and run configure, build, and test as separate commands (no `&&` or pipes). Edit files with the Edit tool.
- Code must compile on macOS with AppleClang 17 (see `CLAUDE.md`): no floating-point `std::from_chars`, no `std::jthread`. Nothing here needs either.
- The decoder round-trip acceptance test runs 1,000,000 slots in every preset; keep `decode()` cheap.
