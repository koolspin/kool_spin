# Feature: Resume state

- **Status:** Approved by the maintainer on 2026-11-12. Protected: build loops can't change it.
- **Roadmap:** Phase 1a (step 6c)
- **Built by:** loop

## Goal

Let a new engine carry on where a crashed one left off. After every block it renders, the engine writes what it needs to resume: each deck's track ID, play state, position, rate, and cue point, and the mixer's settings, at a host time. It writes them to one of two slots in turn, each a seqlock with a generation number and a CRC-32. A new engine built on the same state block picks the newest slot that passes every check, reloads each deck's track by its ID, moves playing decks on by the time that has passed, and fades in over 50 ms. If no slot passes, it starts clean with decks paused, and never crashes.

## Context

- **Decisions that apply:**
  - ADR 0003 §11: the resume state is written every block, alternating between two slots, each a seqlock with a generation number and a CRC-32. A crash during a write leaves the other slot intact; corruption fails the CRC or the range checks, and the engine starts clean with decks paused. A new engine picks the newest valid slot. A deck whose track ID isn't among the tracks passed comes back empty. Playing decks resume where they would be had they kept playing (elapsed time times rate, as during device loss), and fade in over about 50 ms.
  - ADR 0003 §11 lists each deck's gain among what's stored. The channel's trim and fader, in the mixer settings, are that gain. A deck's own play/pause ramp isn't stored: resumed decks start silent and fade in.
  - ADR 0003 §5 and §7: the resume state is part of the engine-state segment, written only by the audio thread, laid out like the other blocks (all `std::uint64_t`, standard-layout, trivially copyable, zero-filled memory valid).
  - ADR 0003 §8: copy, validate, use. The resume-state validator is fuzzed.
  - Roadmap, Phase 1a exit criteria: "A corrupted resume state is rejected: the engine starts clean with decks paused, and never crashes", and the resume-state validator is fuzzed under `fuzz`.
  - The real-time rules in `CLAUDE.md`.
- **The maintainer's design choices** (2026-11-12):
  - 6c builds writing, validating, and resuming. A new `Engine` resumes from a state block in one process, so tests can restart an engine. Phase 1b adds the processes, the supervisor's restarts, and the crash-loop guard;
  - after a resume, the engine fades its whole output in over 50 ms, per sample, on top of the safety stage's own fade-in;
  - a build loop implements it, from the API, stubs, acceptance tests, and fuzz target written first.
- **What exists:**
  - `<ks/ipc/state.h>` (6b): the engine-state layout, version 1, with `DeckState`, `ChannelState`, `MixerState`, and `EngineStatus` seqlocks. `ChannelState` is reused here.
  - `Seqlock`, `SeqlockWriter` (continues from the lock's sequence number), and `SeqlockReader` (`try_read()`, which gives up on a sequence number that stays odd).
  - `Track` has a 64-bit ID (6b). `Deck` has `load()`, `seek()`, `set_rate()`, `set_cue()`, `set_playing()`, and their getters. A silent deck takes a seek, a load, and its rate at once. `kMaxTrackFrames` is 2²⁸.
  - `Mixer` controls clamp as the mixer spec says: trims at most `kMaxTrimDb` (−∞ allowed), levels at most 0 dB (−∞ allowed), faders, crossfader, and cue mix in [0, 1], the crossfader transition in [`kMinCrossfaderTransition`, 1], and only valid bus maps.
  - `Engine` (6a, 6b) holds one reference to each track a deck may still hold, in its places, and releases them through the retire ring and `collect()`. After each rendered block, `rendered()` writes the deck, mixer, and status blocks.
  - `ks_add_fuzz_target()` (`cmake/KsFuzz.cmake`) and the decoder fuzz target, whose checks live in a header that an ordinary test also runs (`tests/acceptance/commands/codec_support.h`).
- **What this adds:** a CRC-32, the resume slots in the engine-state layout (version 2), a snapshot type with its encoder, validator, and slot reader, the engine's writing of a snapshot after every block, and an `Engine` constructor that resumes from a state block.
- **Changes to the state-out spec** (approved with this spec): requirement 7 and the layout criterion (version 2, with the resume slots), requirement 16 (the engine also reads the resume slots when it's built), and requirement 17 (a resume slot is written after the status block). The state-out layout test's version check changes from 1 to 2.
- **The API exists before the code:** the headers named below declare everything, with stubs. The acceptance tests compile against them and fail until the feature is built. The loop implements the stubs. It may add private members and helpers, but must not change the public API. `<ks/ipc/state.h>` is complete, not a stub: it's only a layout.

## Requirements

### CRC-32 (`<ks/ipc/crc32.h>`)

1. **`crc32(std::span<const std::byte>)`** returns the CRC-32 used by zlib, PNG, and Ethernet: reflected polynomial `0xEDB88320`, initial value `0xFFFFFFFF`, final XOR `0xFFFFFFFF`. The CRC of "123456789" is `0xCBF43926`, and of no bytes is 0. It's `constexpr`, `noexcept KS_NONBLOCKING`, and allocates nothing.

### The layout (`<ks/ipc/state.h>`)

2. **`ResumeDeck`**, one deck, in this order, all `std::uint64_t`:

   | Field | Meaning |
   |---|---|
   | `track_id` | The main head's track's ID; 0 with no track. |
   | `flags` | Bit 0 (`kResumePlaying`): playing. Other bits are 0. |
   | `position` | Real: the main head's position, in track frames. |
   | `rate` | Real: the target rate. |
   | `cue` | Real: the cue point, in track frames. |

3. **`ResumeState`**, one slot's value, in this order: `generation`; `host_time` (a host time, encoded as in state-out requirement 1); `decks` (`kStateDecks` `ResumeDeck`s); `channels` (`kStateDecks` `ChannelState`s); `crossfader`, `crossfader_transition`, `main_level_db`, `headphone_level_db`, and `cue_mix` (reals); `bus_main` and `bus_cue`; and last, `checksum`. Mixer fields are encoded as in `MixerState`.
4. **`EngineState`** gains `resume`: `kResumeSlots` (2) `Seqlock<ResumeState>`s, after `status`. `kEngineStateLayoutVersion` becomes 2. Zero-filled memory is still a valid `EngineState`; its resume slots hold nothing valid.
5. Both structs are standard-layout and trivially copyable, with `static_assert`s on their sizes and their fields' offsets.

### Snapshots (`<ks/engine/resume.h>`)

6. **`Snapshot`** is the engine's own, validated, copy of a resume state:
   - `generation` (`std::uint64_t`);
   - `host_time` (`std::chrono::steady_clock::time_point`);
   - `decks`: `kDecks` `DeckSnapshot`s, each with `track_id` (`std::uint64_t`, 0 for none), `playing` (`bool`), and `position`, `rate`, and `cue` (`double`);
   - `mixer` (`MixerSettings`).

   `DeckSnapshot` and `Snapshot` compare equal when every field does. A default `DeckSnapshot` is an empty deck: ID 0, paused, position 0, rate 1, cue 0.
7. **A valid snapshot** has:
   - **generation:** from 1 to 2⁶³ − 1;
   - **host time:** at least 0 nanoseconds since the steady clock's epoch;
   - **each deck:** a rate that's finite, from −`kMaxRate` to `kMaxRate`. With a track ID of 0: paused, position 0, and cue 0. With another ID: a position and a cue that are finite, from 0 to `kMaxTrackFrames`;
   - **the mixer:** settings a `Mixer` can hold: no NaN; trims at most `kMaxTrimDb` and levels at most 0 dB, −∞ allowed; faders, the crossfader, and the cue mix in [0, 1]; the crossfader transition in [`kMinCrossfaderTransition`, 1]; and a valid bus map (both channels even, below `kMaxOutputChannels`, and different).
8. **`encode(const Snapshot&)`** returns the `ResumeState` for a valid snapshot: every field encoded as requirement 3 says, `flags` set only from `playing`, and `checksum` holding, in its low 32 bits, `crc32()` of the bytes of every word before it, in memory order; its high 32 bits are 0. It's `noexcept KS_NONBLOCKING`. The slot a snapshot belongs in is its generation modulo 2.
9. **`validate(const ResumeState& words, std::size_t slot)`** returns the `Snapshot` the words encode, or nothing. It's a pure function of its arguments, `noexcept KS_NONBLOCKING`, and safe for any bytes. It accepts the words only if all of these hold:
   - `slot` is 0 or 1, and the generation modulo 2 equals it;
   - the checksum is right, with its high 32 bits 0;
   - `flags` has no bit but bit 0, and each side and channel cue is a code that `MixerState` allows;
   - the snapshot they encode is valid (requirement 7).

   **Round trip:** for any words it accepts, `encode()` of the result gives back the same words, bit for bit. For any valid snapshot `s`, `validate(encode(s), s.generation % 2)` returns `s`, and the other slot number returns nothing.
10. **`read_snapshot(ipc::EngineState&)`** copies each resume slot with a `SeqlockReader` (a slot whose sequence number stays odd, as a crash mid-write leaves it, yields nothing), validates each copy with its slot number, and returns the valid snapshot with the higher generation, or nothing if neither is valid. It reads each slot once and writes nothing. It isn't real-time: it's for building an engine.

### Writing (`<ks/engine/engine.h>`)

11. **Every block:** in `rendered()`, after the status block, an engine built with a state block writes one snapshot of what it holds after the block, to slot `generation % 2`, through a `SeqlockWriter`:
    - `generation`: one more than the last it wrote (see requirement 12 for the first);
    - `host_time`: the block's end, as for the status block;
    - **each deck:** the main head's track's ID, and with a track, whether it's playing, its position, and its cue point; with no track, ID 0, paused, position 0, and cue 0, whatever the deck's own flags say. The rate is the deck's target rate either way;
    - `mixer`: `Mixer::settings()`.

    It writes nothing for blocks the render core doesn't render, and nothing before the first. Each snapshot it writes is valid, so it reads back with `validate()` as what was written.
12. **Generations continue across engines:** when an engine is built with a state block, with either constructor, it calls `read_snapshot()` once. Its first snapshot's generation is one more than the snapshot found, or 1 if there's none. So after any number of restarts, the newest snapshot is always the one with the highest generation.

### Resuming (`<ks/engine/engine.h>`)

13. **`Engine(ipc::EngineState& state, std::span<const Track* const> tracks, const BusMap& map = {})`** builds the engine as the state-block constructor does, then resumes from `read_snapshot(state)`:
    - **With no snapshot,** it's the same as `Engine(state, map)`: every deck empty and paused, the default mixer settings with `map` (or the default map if `map` is invalid), no tracks taken, and no fade.
    - **With a snapshot,** the mixer gets every setting in it, including its bus map (`map` is ignored), and each deck gets its rate. For a deck with a nonzero track ID, the engine looks for the first non-null track in `tracks` with that ID. If there is one, and `Deck::load()` accepts it, the deck loads it, takes its cue point, and the engine holds one reference to it, as if `load()` had given one. Otherwise the deck comes back empty. Tracks with ID 0 never match.
    - Everything is done by the time the constructor returns. It doesn't wait for, or need, the audio thread.
14. **`resume_outcome()`** returns a `ResumeOutcome`: the generation of the snapshot the engine resumed from (empty if it started clean), and for each deck the track it holds a reference to (null if none). The engine releases these references through `collect()`, as it does for loaded tracks. An engine built with any other constructor reports an empty outcome. The outcome never changes after construction, so any thread may read it.
15. **The first block:** the first time `process()` renders a block (one it doesn't ignore), before its handovers and commands, the engine moves each deck that resumed a track to the snapshot's position, plus, if it was playing, `rate × track sample rate × elapsed`, where `elapsed` is the block's `host_time` minus the snapshot's `host_time`, in seconds, and 0 if negative. The deck clamps the result to its track, so a deck that would have run off an end stops there. Then each deck that was playing plays. Paused decks stay paused, at their position. Handovers and commands in the same block apply after this.
16. **The fade-in:** after resuming from a snapshot, the engine multiplies every channel of its output, after the mixer, by a gain that starts at 0. Counting frames from the first frame of the first block, frame `n` has gain `n / N`, computed in `double`, where `N` is `kResumeFade` (50 ms) at the first block's sample rate, rounded to the nearest frame. From frame `N` on, the gain is 1 and the engine no longer applies it. The count carries across blocks.
17. **Block size:** a resumed engine's output doesn't depend on how its frames are split into blocks, given the same first host time, handovers, and commands (ADR 0003 §3).

### Real time and fuzzing

18. **Real time:** everything `process()` and `rendered()` call, including encoding a snapshot, the CRC, the first block's moves, and the fade, is `KS_NONBLOCKING`, allocates nothing, and takes no lock. The constructors and `read_snapshot()` run on the control thread.
19. **Fuzzing:** under the `fuzz` preset, a libFuzzer target runs for 1,000,000 inputs with a fixed seed. For each input it:
    - validates the input's bytes as a `ResumeState`, zero-padded, for slot 0 and slot 1: whatever it accepts is a valid snapshot (requirement 7) and encodes back to the same bytes;
    - does the same with the checksum corrected, so the range checks are reached;
    - builds a valid snapshot from the input's bytes and checks the round trip of requirement 9;
    - fills both resume slots of an `EngineState`, sequence numbers included, from the input's bytes, and checks that `read_snapshot()` returns the newer of the slots whose sequence number is even and whose words `validate()` accepts, or nothing if there's none.

    The checks live in a header that an ordinary test also runs, on 100,000 inputs from a fixed seed, in every preset.

## Acceptance criteria

The tests are in `tests/acceptance/resume-state/`. They render through a `RenderCore` at 48 kHz, in blocks of 64 frames on 4 zero-filled channels, unless a criterion says otherwise. The state block is a value-initialized `ipc::EngineState` on the heap. "Engine A" is the engine that writes the state block; "engine B" is built from the same block afterwards, as a restarted engine would be.

The CRC and the layout (`layout_test.cpp`):

- [ ] `[auto]` **CRC-32:** the CRC of "123456789" is `0xCBF43926`, of "The quick brown fox jumps over the lazy dog" is `0x414FA339`, and of no bytes is 0; the first is also checked in a `static_assert`. (test: "CRC-32: check values")
- [ ] `[auto]` **Layout:** `ResumeDeck` and `ResumeState` have the fields of requirements 2 and 3, in order, at 8-byte steps from offset 0, and are standard-layout and trivially copyable; `EngineState::resume` holds `kResumeSlots` (2) seqlocks right after `status`; `kEngineStateLayoutVersion` is 2. (test: "Resume state: the layout")

Snapshots (`snapshot_test.cpp`):

- [ ] `[auto]` **Encoding:** a snapshot with distinct values in every field encodes each one where requirement 3 puts it (reals and host times as bit patterns, `flags` from `playing`), and the checksum is the CRC of the words before it. (test: "Resume state: encoding")
- [ ] `[auto]` **Round trip:** valid snapshots, including every limit of requirement 7 (rates ±4, positions 0 and `kMaxTrackFrames`, −∞ trims and levels, transition 0.01, generations 1, 2, and 2⁶³ − 1, host time 0), validate back to themselves in their own slot, and to nothing in the other. (test: "Resume state: valid snapshots round-trip")
- [ ] `[auto]` **Corruption:** starting from a valid encoded snapshot, each of these is rejected:
  - every single-bit flip of every word;
  - a wrong slot number, a slot number of 2, generation 0, and generation 2⁶³;
  - a checksum with a high bit set;
  - and, each with the checksum recomputed: a negative host time; a flags bit other than bit 0; an empty deck that's playing or has a nonzero position or cue; NaN, ±∞, negative, and over-`kMaxTrackFrames` positions and cues; NaN, ±∞, and out-of-range rates; NaN and +∞ trims, and trims over 12 dB; levels over 0 dB; faders, crossfader, and cue mix outside [0, 1]; transitions outside [0.01, 1]; side 3; channel cue 2; and bus maps that are odd, equal, or 8 or more.

  (test: "Resume state: every corruption is rejected")
- [ ] `[auto]` **Choosing a slot:** with both slots valid, `read_snapshot()` returns the higher generation; with the newest slot corrupted, or left mid-write (an odd sequence number), it returns the older; with neither valid, or a zero-filled state block, it returns nothing. (test: "Resume state: the newest valid slot wins")
- [ ] `[auto]` **Validation properties:** the fuzz target's checks (requirement 19) pass on 100,000 inputs from a fixed seed. (test: "Resume state: validation properties")

The engine (`engine_resume_test.cpp`):

- [ ] `[auto]` **Writing:** while engine A loads, plays, seeks, sets cue points and rates, unloads, and changes every mixer setting through commands, after every block slot `g % 2` validates to generation `g` with the block's end as its host time, each deck's track ID, play state, position, rate, and cue point as the engine's decks report them (empty decks as requirement 11 says), and `Mixer::settings()`; the other slot still holds generation `g − 1`. Generations start at 1 in a zero-filled block. (test: "Resume: the engine writes a snapshot after every block")
- [ ] `[auto]` **Generations continue:** an engine built on a block whose newest valid snapshot has generation `g`, with either constructor, writes `g + 1` first; one built on a block with no valid snapshot writes 1 first. (test: "Resume: generations continue across engines")
- [ ] `[auto]` **Carrying on:** engine A plays deck 0 at rate 1 (a 48 kHz track), deck 1 at rate 1.0159 (a 44.1 kHz track), and deck 2 in reverse at rate −0.5, holds deck 3 paused with a cue point, and sets every mixer control. Engine B is built from A's block and the tracks, and its first block starts 0.75 s after A's last block ended. B's outcome names A's generation and each deck's track; after B's first block, each deck's position is the snapshot's position plus `rate × track rate × 0.75 s` plus one block's travel (within 10⁻⁴ frames), each deck's rate, cue point, play state, and track ID match A's, and B's mixer settings equal A's, including A's bus map, although B was given a different one. (test: "Resume: decks carry on where they would have been")
- [ ] `[auto]` **No gap:** engine A plays two tracks through the crossfader; engine B resumes from a copy of A's state block, with its first block starting where A's last ended, while A also carries on. From frame `N` (50 ms) on, B's output equals A's within 10⁻⁵. (test: "Resume: with no gap, the output carries on")
- [ ] `[auto]` **Fade-in:** a deck playing a constant 0.5 resumes. B's first frame is silent; at every frame `n` before `N`, each sample's magnitude is at most `0.5 × n / N` plus 10⁻³; at frame `N` and after, it's 0.5 within 10⁻³. With a 44.1 kHz device, `N` is 2,205 frames. (test: "Resume: playback fades in over 50 ms")
- [ ] `[auto]` **Missing tracks:** when engine B is given only some of A's tracks, plus an unrelated one, a track with ID 0, a null pointer, and two tracks sharing an ID, decks whose track wasn't given come back empty and paused, the first track with a matching ID is taken, and nothing else is: the outcome names exactly the tracks taken. (test: "Resume: decks without their track come back empty")
- [ ] `[auto]` **Ends:** a deck playing forward 1,000 frames from its end, and one playing in reverse 1,000 frames from its start, resume after 1 s at the end and at frame 0, still flagged as playing. (test: "Resume: decks stop at the ends")
- [ ] `[auto]` **Corruption starts clean:** for each of these, applied to both slots of A's block: zero-filled, all bytes 0xFF, bytes from a fixed seed, a flipped bit, odd sequence numbers, and out-of-range values with a correct checksum (a NaN position, rate 10, fader 2, bus map 1 and 3). Engine B, given the tracks, has an empty outcome, takes no reference (`collect()` returns nothing), has every deck empty and paused and the default mixer settings with the map it was given, renders 100 blocks of finite silence, and writes generation 1 first. With only the newest slot corrupted, B resumes from the older one. (test: "Resume: a corrupted resume state starts clean")
- [ ] `[auto]` **References:** a track resumed on two decks is named twice in the outcome; unloading both returns it twice through `collect()`; and loads, unloads, and commands work as usual afterwards. (test: "Resume: references come back through collect()")
- [ ] `[auto]` **Block size:** a resumed engine rendered in 64-frame blocks and in blocks of varying sizes from 1 to 512 frames, from the same first host time and with the same commands at the same frames, gives bit-identical output, fade included. (test: "Resume: output doesn't depend on block size")

Real time and fuzzing:

- [ ] `[auto]` **Real time:** rendering a resumed engine through `try_render()`, with its first-block moves, its fade, commands, and snapshot writes, compiles under function-effect analysis and is clean under `rtsan`. (test: `realtime_test.cpp`, "Resume is real-time safe")
- [ ] `[auto]` **Fuzzing:** under `fuzz`, `ks_resume_state_fuzz` runs 1,000,000 inputs with seed 1 without a failed check, a crash, or a sanitizer report. (test: `resume_fuzz.cpp`, ctest `ks_resume_state_fuzz`)
- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux and on macOS arm64 (`debug`, `release`, `asan`, `tsan`).

## Out of scope

- Processes: the supervisor passing segments and tracks to a new engine, deciding when to restart, the crash-loop guard (which builds the engine with `Engine(state, map)` to start clean), and measuring restart time (Phase 1b).
- Reading client rings' tails back and checking them after a restart (ADR 0003 §11, "Rings"; Phase 1b).
- A load, seek, or handover in flight when the engine stopped: the snapshot holds only what the decks hold.
- The render core's mute and output state, and the safety stage's state: a new render core starts as it always does.
- Keeping state across sessions (the session journal, Phase 1b).

## Open questions

None.
