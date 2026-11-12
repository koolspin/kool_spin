# Feature: Commands in

- **Status:** Approved by the maintainer on 2026-11-11. Protected: build loops can't change it.
- **Roadmap:** Phase 1a (step 6a)
- **Built by:** loop

## Goal

Let other threads, and later other processes, control the engine without ever making the audio thread wait. Clients write fixed-size commands into rings that the engine reads at the start of each block; a pure, fuzzed decoder turns each one into a validated command or a rejection. The control thread hands tracks and client rings to the audio thread through a give ring, and gets them back through a retire ring once nothing on the audio thread uses them.

## Context

- **Decisions that apply:**
  - ADR 0003 §4: one ring per client, 64-byte slots, absolute values, a fixed number of commands per ring per block, visited in a fixed order, and the last command applied wins. Loads don't go through client rings.
  - ADR 0003 §8: copy, validate, use. The command decoder is a fuzz target.
  - ADR 0003 §10: everything the audio thread uses is allocated before it runs. Objects reach it through the give ring and leave through the retire ring, which the control thread drains and frees.
  - ADR 0003 §3: live commands take effect at the start of the next block, and output doesn't depend on block size.
  - The real-time rules in `CLAUDE.md`.
- **The maintainer's design choices** (2026-11-11):
  - step 6 is three pull requests: 6a, commands in (this spec); 6b, state out; 6c, the resume state;
  - a build loop implements 6a;
  - real values travel as the bit patterns of IEEE-754 doubles;
  - the engine acknowledges every command it reads, valid or not, and counts rejections. 6b publishes these counts; 6a keeps them.
- **What exists:**
  - `SpscRing`, `RingProducer`, and `RingConsumer` (`<ks/ipc/spsc_ring.h>`): a lock-free single-producer, single-consumer ring that works in shared or private memory. A handle that finds the counters inconsistent marks the ring corrupt and stops using it.
  - `Mixer` (`<ks/engine/mixer.h>`), a `Graph` that owns 4 decks and validates every control. `Deck::holds()` tells whether a deck still refers to a track, including during a crossfade or a waiting load.
  - `RenderCore` runs one `Graph`. Nothing reads commands yet: tests call the mixer's and decks' methods directly.
  - The `fuzz` preset builds with libFuzzer's coverage instrumentation, ASan, and UBSan, but there are no fuzz targets yet.
- **What this adds:** the command slot, the decoder, and `Engine`, the graph that the render core will run from now on. `Engine` owns the interpolator and the mixer, reads the client rings, and handles the give and retire rings.
- **The API exists before the code:** the headers named below declare everything, with stubs. The acceptance tests and the fuzz target compile against them and fail until the feature is built. The loop implements the stubs. It may add private members and helpers, but must not change the public API.
- **Who calls what:** a "client" is anything that writes a command ring: in 1a, tests and the development harness; in 1b, other processes. The "control thread" is the one thread that calls `Engine`'s control methods; in 1a, the harness's main thread or a test's.

## Requirements

### The command slot (`<ks/ipc/command.h>`)

1. **The layout.** `CommandSlot` is a standard-layout, trivially copyable struct of exactly 64 bytes:

   | Offset | Field | Type | Meaning |
   |---|---|---|---|
   | 0 | `sequence` | `std::uint64_t` | Chosen by the client. The engine reports the last one it read. |
   | 8 | `type` | `std::uint32_t` | A `CommandType`, kept as an integer: another process wrote it. |
   | 12 | `target` | `std::uint32_t` | The deck or channel, for the commands that take one. |
   | 16 | `payload` | `std::array<std::uint64_t, 6>` | The arguments. |

   - `CommandRing` is `SpscRing<CommandSlot, kCommandRingCapacity>`, with `kCommandRingCapacity` = 256.
   - A zero-filled slot is invalid: no command type is 0.
2. **Command types and their encoding:**

   | Type | Value | Target | `payload[0]` | `payload[1]` |
   |---|---|---|---|---|
   | `SetPlaying` | 1 | deck | 0 (pause) or 1 (play) | 0 |
   | `SetRate` | 2 | deck | rate, as a double | 0 |
   | `Seek` | 3 | deck | track frame, as a double | 0 |
   | `SetCuePoint` | 4 | deck | track frame, as a double | 0 |
   | `SetTrim` | 5 | channel | dB, as a double; −∞ allowed | 0 |
   | `SetFader` | 6 | channel | position, as a double | 0 |
   | `SetCrossfaderAssign` | 7 | channel | 0 (A), 1 (B), or 2 (thru) | 0 |
   | `SetChannelCue` | 8 | channel | 0 (off) or 1 (on) | 0 |
   | `SetCrossfader` | 9 | 0 | position, as a double | 0 |
   | `SetCrossfaderCurve` | 10 | 0 | transition, as a double | 0 |
   | `SetMainLevel` | 11 | 0 | dB, as a double; −∞ allowed | 0 |
   | `SetHeadphoneLevel` | 12 | 0 | dB, as a double; −∞ allowed | 0 |
   | `SetCueMix` | 13 | 0 | position, as a double | 0 |
   | `SetBusMap` | 14 | 0 | the main bus's first channel | the cue bus's first channel |

   - "As a double" means the value's IEEE-754 bit pattern, `std::bit_cast<std::uint64_t>(value)`.
   - Channel i is deck i (mixer requirement 1).
   - `payload[2]` to `payload[5]` are always 0, and so is `payload[1]` except for `SetBusMap`.

### Decoding (`<ks/engine/command.h>`)

3. **`decode(const ipc::CommandSlot&)`** returns `std::expected<Command, Rejection>`. It's a pure function of the slot's bytes, `noexcept KS_NONBLOCKING`, and safe for any 64 bytes.
   - **`Command`** holds the type, and the arguments its type uses: `deck` (for deck and channel commands), `value` (a double), `on` (for `SetPlaying` and `SetChannelCue`), `side` (a `CrossfaderSide`), and `bus_map` (a `BusMap`). Fields the type doesn't use hold their defaults: deck 0, value 0, off, `Thru`, and `BusMap{}`. Commands compare equal when every field does.
   - **It checks, in this order,** and returns the first failure:
     1. `UnknownType`: the type isn't one in requirement 2.
     2. `BadTarget`: a deck or channel command whose target is `kDecks` or more, or another command whose target isn't 0.
     3. `NonzeroPadding`: a payload word that requirement 2 says is 0 isn't.
     4. `BadValue`: the argument isn't allowed (requirement 4).
4. **Allowed values:**
   - **Doubles:** NaN is rejected, and so is +∞. −∞ is rejected except for `SetTrim`, `SetMainLevel`, and `SetHeadphoneLevel`. Every finite value is accepted, out of range or not: the controls clamp them, as the deck and mixer specs say. A controller that overshoots still moves the control to its end.
   - **Flags:** exactly 0 or 1.
   - **Sides:** exactly 0, 1, or 2.
   - **Bus maps:** a valid map as mixer requirement 2 defines it: both channels even, below `kMaxOutputChannels`, and different.
5. **`apply(const Command&, Mixer&)`** calls the matching deck or mixer control with the command's arguments. `SetPlaying` calls `Deck::set_playing()`, `Seek` calls `Deck::seek()`, `SetCuePoint` calls `Deck::set_cue()`, `SetChannelCue` calls `Mixer::set_cue()`, and the rest call the control of the same name. It's `noexcept KS_NONBLOCKING`.

### The engine (`<ks/engine/engine.h>`)

6. **`Engine`** is a final `Graph`.
   - **It owns** an `Interpolator` and a `Mixer`. `Engine(const BusMap& = {})` builds them; everything the audio thread uses is allocated by the time it returns.
   - **`mixer()`** returns the mixer, for reading state. Call it only on the thread that calls `process()`, between blocks.
   - **Constants:** `kMaxClients` = 8 client rings; `kCommandsPerBlock` = 32, the most commands read from one ring in one block; `kHandoverCapacity` = 64, the capacity of the give ring and of the retire ring.
7. **The control methods:** `load()`, `unload()`, `attach()`, `detach()`, and `collect()`. `collect()` pops one release from the retire ring, if there is one: a `Released` that points to either a track or a ring.
   - **Threads:** they may run while another thread calls `process()`, but only one thread at a time may call them.
   - **They never wait** for the audio thread: each pushes to the give ring, or pops from the retire ring, and returns.
   - **Not real-time:** they're `noexcept`, but aren't called from the audio thread, so they don't need `KS_NONBLOCKING`.
8. **Each block,** `process()` does these, in order, then nothing else:
   1. **Handovers:** takes items from the give ring, oldest first, and applies them (requirements 10 and 12). It stops at an item that has to wait (requirement 13).
   2. **Commands:** visits the attached clients in slot order, 0 to `kMaxClients` − 1, and reads up to `kCommandsPerBlock` commands from each (requirement 9).
   3. **Mixing:** calls the mixer's `process()` with the block.
   4. **Releases:** pushes releases to the retire ring (requirements 11 and 12).

   If the block has zero frames, more than `kMaxBlockFrames`, or a sample rate of 0, `process()` does nothing at all.
9. **Reading a command:** for each slot it pops from a client's ring, the engine:
   - counts it as read, and records its sequence number as the client's last;
   - decodes it, and applies it if it's valid;
   - if it's rejected, counts the rejection and records the reason. A rejected command changes nothing else.

   A ring whose consumer finds it corrupt is read no further: the client is marked corrupt until it's detached. The other clients carry on.

   **`client_stats(i)`** returns client slot i's `ClientStats`: whether it's attached, whether it's corrupt, the commands read, the last sequence number read (0 before the first), the commands rejected, and the last rejection's reason (empty before the first). A slot's statistics start from zero when a client is attached to it. Call it only where `mixer()` may be called; 6b publishes these numbers through the state block.
10. **Tracks:**
    - **`load(deck, track)`** asks for `track` to be loaded on `deck`. It returns false, and gives nothing, if `deck` is `kDecks` or more, or the give ring is full. Otherwise the engine calls `Deck::load()` in a later block's handovers.
    - **`unload(deck)`** does the same for `Deck::unload()`.
    - **References:** each successful `load()` gives the engine one reference to the track. The caller keeps ownership, and must keep the track alive until every reference it gave has been released.
11. **Releasing tracks:**
    - **Each reference is released exactly once,** as a `Released` whose `track` points to it, from `collect()`.
    - **When:** in the releases at the end of a block, once the deck it was loaded on no longer holds the track (`Deck::holds()`). That includes a load that `Deck::load()` refused, and a waiting load that a later load replaced.
    - **Duplicates:** if a deck gets a reference to a track it already has a reference to, the older one is released in that block's releases, and the newer one takes its place. A deck therefore keeps at most one reference per track it holds.
    - **Room:** each deck has room for 4 references: the 3 tracks it can hold at once (the outgoing head's, the main head's, and a waiting load's), and one more.
    - **So:** once a track's references have all been released, no deck holds it.
12. **Clients:**
    - **`attach(ring)`** chooses a free client slot, the lowest one, and asks the engine to start reading `ring` from its current tail. It returns the slot, or nothing if all `kMaxClients` slots are in use or the give ring is full. A slot is in use from `attach()` until the ring's release has been collected.
    - **`detach(slot)`** asks the engine to stop reading that slot's ring. It returns false, and gives nothing, if the slot isn't attached or the give ring is full.
    - **Releasing rings:** in the handovers that apply a detach, the engine stops reading the ring. In that block's releases, it releases the ring, as a `Released` whose `ring` points to it. After that the engine never touches the ring again, and the caller may free it.
13. **When rings fill up:**
    - **The give ring:** `load()`, `unload()`, `attach()`, and `detach()` return false or nothing, as above, and the caller can try again later.
    - **The retire ring:** releases that don't fit wait for a later block, in the order they would have been released. None is lost.
    - **A deck with no room** for another reference makes a load for it wait. That happens only while releases are waiting for the retire ring. The handover stays at the front of the give ring, and the items behind it wait too, so handovers apply in the order they were given.
14. **Block-size invariance.** Given the same handovers and commands before the same frames, the output doesn't depend on how the frames were divided into blocks (ADR 0003 §3).

### Fuzzing

15. **The decoder is fuzzed.** Under the `fuzz` preset, a libFuzzer target exercises `decode()`, and `ctest` runs it for 1,000,000 inputs with a fixed seed. CMake adds fuzz targets with a `ks_add_fuzz_target()` function, which builds nothing in other presets.
    - **Bytes to command:** the first 64 bytes of the input, zero-padded, form a slot. If it decodes, encoding the result with the same sequence number gives back exactly the same 64 bytes.
    - **Command to bytes:** the input also selects a valid command. Encoding it and decoding the result gives back the same command.
    - **No sanitizer report:** ASan and UBSan stop the run at the first error.

## Acceptance criteria

The tests are in `tests/acceptance/commands/`. They render at 48 kHz in blocks of 64 frames on 4 zero-filled channels, unless a criterion says otherwise. Decks play the constant tracks from the mixer tests, or the synthetic track from the deck tests. The test support provides `encode(sequence, Command)`, written from requirement 2 independently of the decoder.

The command slot and the decoder (`decoder_test.cpp`):

- [ ] `[auto]` `CommandSlot` is 64 bytes, standard layout, and trivially copyable, with the offsets in requirement 1. `CommandRing`'s capacity is 256. (test: "Commands: the slot layout")
- [ ] `[auto]` **Each type decodes:** for every row of requirement 2, a hand-built slot decodes to the expected `Command`, including −∞ for trim and both levels, and out-of-range values that the control will clamp (fader 2, rate 10, seek −5, trim +20 dB). (test: "Commands: every type decodes")
- [ ] `[auto]` **Rejections,** each with its reason:
  - types 0, 15, 0x101 (its low byte alone would be `SetPlaying`, so the decoder must check all 32 bits before narrowing), and 0xFFFFFFFF;
  - deck 4 for a deck command, channel 4 for a channel command, and target 1 for a global one;
  - a nonzero `payload[1]` (except for `SetBusMap`) and a nonzero `payload[5]`;
  - NaN for every double (quiet, signalling, and negative), +∞ for every double, and −∞ for the doubles that don't allow it;
  - flag 2, side 3, and bus maps {1, 2}, {2, 2}, and {8, 0}.

  When a slot has several faults, the reason is the first in requirement 3's order. (test: "Commands: invalid slots are rejected")
- [ ] `[auto]` **Round trips:** 1,000,000 slots from a seeded generator (random bytes, and valid slots with one byte changed) keep requirement 15's two properties. This runs in every preset. (test: "Commands: decoding and encoding round-trip")

The engine (`engine_test.cpp`):

- [ ] `[auto]` **Commands match direct calls:** a script of every command type, sent through a client ring, gives output identical, bit for bit, to the same controls called directly on a `Mixer` before the same blocks, and the same `settings()` and deck state. (test: "Engine: commands do what direct calls do")
- [ ] `[auto]` **Statistics:** after valid and invalid commands, `client_stats()` shows the commands read, the last sequence number, the rejections, and the last reason. A rejected command changes no setting. (test: "Engine: every command read is counted")
- [ ] `[auto]` **Fairness:**
  - with 100 commands waiting in each of two rings, each block reads 32 from each;
  - when clients 0 and 1 set the same fader in the same block, client 1's value wins;
  - a ring found corrupt marks its client corrupt and is read no further, while the other client's commands still apply.

  (test: "Engine: rings are read fairly, in slot order")
- [ ] `[auto]` **Loading:** a track loaded with `load()` plays after a play command. Loading a second track releases the first once the 5 ms crossfade is over, and not before. `unload()` releases the last one after its fade. A track with an invalid sample rate is refused by the deck and released in the same block. (test: "Engine: loads hand tracks over and release them")
- [ ] `[auto]` **References:** a script of 1,000 random loads and unloads across the 4 decks, using 3 tracks and sometimes the same track on several decks, rendered in blocks of 1 to 300 frames:
  - every load is released exactly once;
  - after every block, a track with no references outstanding isn't held by any deck;
  - at the end, after unloading everything and rendering 1,000 more frames, every reference has been released.

  (test: "Engine: every reference is released once, after its deck lets go")
- [ ] `[auto]` **Back pressure:**
  - `load()` returns false once 64 handovers are waiting;
  - with `collect()` never called, releases wait once the retire ring is full, and later loads wait too, in order;
  - while no release is waiting, a burst of 8 different tracks to one deck, paused or playing, applies in one block, and the tracks the deck lets go of are released in that block;
  - after `collect()` runs, everything proceeds, and every reference is released exactly once.

  (test: "Engine: full rings delay, but lose nothing")
- [ ] `[auto]` **Clients:**
  - an attached ring's commands apply from the next block;
  - `attach()` fails when 8 clients are attached;
  - after `detach()`, the ring is released and its commands are no longer read;
  - the slot is reused only after the release has been collected.

  (test: "Engine: clients attach and detach")
- [ ] `[auto]` **Block-size invariance:** a script that loads two tracks, plays them, and sends crossfader, fader, rate, seek, and cue commands at scheduled frames, then, while a deck plays, loads a burst of 6 different tracks with a play command in one block, renders bit-identical output with blocks of 1, 37, 128, and 512 frames. The play command reaches the burst's last track, so the deck is still audible at the end. (test: "Engine: output doesn't depend on block size")
- [ ] `[auto]` **Under load:** one thread renders blocks without pause for 2 seconds, while another loads, unloads, attaches, detaches, sends commands, and collects as fast as it can. Afterwards, every reference and every detached ring has been released exactly once. Under the `tsan` preset, ThreadSanitizer reports nothing. (test: "Engine: the control thread and the audio thread share nothing unsafely")

Real-time safety and fuzzing:

- [ ] `[auto]` `decode()`, `apply()`, and `Engine::process()` are called from a `KS_NONBLOCKING` function, so function-effect analysis checks them at compile time. Under the `rtsan` preset, RealtimeSanitizer checks them at run time in every test that renders. (test: `realtime_test.cpp`, "Commands are real-time safe")
- [ ] `[auto]` Under the `fuzz` preset, the decoder fuzz target runs 1,000,000 inputs with requirement 15's properties and no sanitizer report. (test: `decoder_fuzz.cpp`, run by `ctest` as `ks_command_decoder_fuzz`)

Everywhere:

- [ ] `[auto]` All of the above pass in every CI configuration: every preset on Linux, and `debug`, `release`, `asan`, and `tsan` on macOS arm64.

There are no `[manual]` criteria: nothing here is audible that the mixer's listening test didn't already cover.

## Out of scope

- Publishing anything: deck, mixer, and engine status blocks, the acknowledgements, and the visual play position (6b).
- The resume state (6c).
- Segments, segment headers, and layout versions for command rings, sockets, and other processes (Phase 1b). In 1a, rings live in private memory or in anonymous regions.
- A client library that encodes commands. Tests have their own encoder; the development harness (step 7) will need one.
- Loading or ejecting tracks through a client ring.
- Commands scheduled for a frame inside a block. Live commands apply at the start of the next block; tests split blocks to schedule them.
- Using `Engine` in `OutputController` or the development harness (step 7).

## Open questions

None.
