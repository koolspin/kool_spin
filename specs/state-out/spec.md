# Feature: State out

- **Status:** Approved by the maintainer on 2026-11-11. Protected: build loops can't change it.
- **Roadmap:** Phase 1a (step 6b)
- **Built by:** loop

## Goal

Let other threads, and later other processes, see what the engine is doing without ever making the audio thread wait. After each block it renders, the engine publishes each deck's state, the mixer's settings, and its own status, including what it has read from each client, through seqlocks in an engine-state block. Each deck's state carries a visual play position: a track frame, the host time at which that frame is due, and the output latency after that.

## Context

- **Decisions that apply:**
  - ADR 0003 §5: state goes out through seqlocks, one block per deck, one for the mixer, and one for engine status (device state, underrun and NaN counts, callback load, heartbeat, and what was read from each client). The audio thread is the only writer, and no real-time thread reads a seqlock.
  - ADR 0003 §3 and §5: the visual play position is a track frame and the host time at which it reaches the output.
  - ADR 0003 §7: shared layouts are standard-layout, trivially copyable, fixed-width integers, with a segment header. The engine-state segment is `SegmentKind::EngineState`.
  - ADR 0003 §11: the heartbeat counts rendered blocks, whichever driver renders them.
  - The real-time rules in `CLAUDE.md`.
- **The maintainer's design choices** (2026-11-11):
  - step 6 is three pull requests: 6a, commands in (merged); 6b, state out (this spec); 6c, the resume state;
  - a build loop implements 6b;
  - the visual play position is published raw: the block's host time, unsmoothed, and the output latency the driver reports. The delay-locked loop and the device-latency estimate come with the waveform view (Phase 3), which measures the error;
  - tracks get a 64-bit ID from whoever creates them (the supervisor, in 1b). Deck state publishes it, and 6c's resume state reuses it;
  - no meters yet: the mixer block publishes the settings. Meters come with the UI that needs them.
- **What exists:**
  - `Seqlock<T>`, `SeqlockWriter<T>`, and `SeqlockReader<T>` (`<ks/ipc/seqlock.h>`), stress-tested on macOS arm64. `T` is copied in and out as 64-bit words.
  - `Region` and `SegmentHeader` (`<ks/ipc/region.h>`). `SegmentKind::EngineState` exists, with no layout yet.
  - `RenderCore` runs one `Graph` for whichever driver is active, then the safety stage. It already counts frames rendered, NaN and infinite samples, and limited frames, and reports `rendered_until()`, mute, and silence.
  - `Engine` (6a) is the graph: four decks and the mixer, client command rings, and per-client statistics (`client_stats()`).
  - `OutputController` (control thread) plays through a device, freewheels while it's lost, and hands back when it returns. Its `OutputState` is visible only on the control thread.
  - `ks::core::monotonic_now()` reads the monotonic clock from `KS_NONBLOCKING` code. It holds the only approved function-effects suppression.
- **What this adds:** the engine-state layout, a track ID, an output latency on each block, render timing and the output state in the render core, a hook through which the render core tells its graph about each rendered block, and the engine's publishing of every state block from that hook.
- **Underruns:** miniaudio can't report device underruns on most backends. The engine can measure the part it causes: a block it takes longer to render than the block lasts. The soak test's "no buffer underruns caused by the engine" reads this count.
- **The API exists before the code:** the headers named below declare everything, with stubs. The acceptance tests compile against them and fail until the feature is built. The loop implements the stubs. It may add private members and helpers, but must not change the public API. `<ks/ipc/state.h>` is complete, not a stub: it's only a layout.

## Requirements

### The engine-state layout (`<ks/ipc/state.h>`)

1. **Encoding:** every field is a `std::uint64_t`.
   - **Reals** are the bit pattern of an IEEE-754 double, as in commands.
   - **Host times** are nanoseconds since the steady clock's epoch, as the two's-complement bit pattern of a `std::int64_t`. **Durations** are nanoseconds.
   - **Flags** are bits, listed below; other bits are 0.
   - **Codes:** a crossfader side is 0 for A, 1 for B, 2 for thru (as in commands). An output state is 0 stopped, 1 playing, 2 freewheeling, 3 attaching. A rejection is 0 for none, then 1 `UnknownType`, 2 `BadTarget`, 3 `NonzeroPadding`, 4 `BadValue`.
2. **Sizes:** `kStateDecks` is 4 and `kStateClients` is 8, equal to `engine::kDecks` and `engine::kMaxClients`.
3. **`DeckState`**, one deck, in this order:

   | Field | Meaning |
   |---|---|
   | `heartbeat` | The status heartbeat of the block that published it. |
   | `flags` | Bit 0: a track is on the main head. Bit 1: playing. Bit 2: waiting at the decode limit (`Deck::waiting()`). Bit 3: the main head's track is completely decoded. |
   | `track_id` | The main head's track's ID; 0 with no track. |
   | `track_sample_rate` | Its sample rate; 0 with no track. |
   | `track_frames` | Its capacity in frames; 0 with no track. |
   | `frames_ready` | Its decoded frames (`Track::ready()`); 0 with no track. |
   | `position` | Real: `Deck::position()` after the block, in track frames. |
   | `rate` | Real: `Deck::rate()`, the target rate. |
   | `cue` | Real: `Deck::cue()`. |
   | `host_time` | Host time: when the frame at `position` is due at the driver, the end of the block (`Block::host_time` plus the block's duration). |
   | `output_latency` | Duration: the block's `output_latency`. The frame at `position` is heard at `host_time` + `output_latency`. |

4. **`MixerState`**, in this order: `heartbeat`; for each of the 4 channels, `trim_db` (real), `fader` (real), `side` (code), and `cue` (0 or 1); then `crossfader`, `crossfader_transition`, `main_level_db`, `headphone_level_db`, and `cue_mix` (reals); then `bus_main` and `bus_cue`. These are `Mixer::settings()` after the block.
5. **`ClientAck`**, one client slot, in this order: `flags` (bit 0 attached, bit 1 corrupt), `commands_read`, `last_sequence`, `rejected`, `last_rejection` (code). These are `Engine::client_stats()` after the block.
6. **`EngineStatus`**, in this order:

   | Field | Meaning |
   |---|---|
   | `heartbeat` | Blocks rendered, this one included (`RenderCore::blocks_rendered()`). |
   | `frames_rendered` | `RenderCore::frames_rendered()`. |
   | `sample_rate` | The block's sample rate. |
   | `host_time` | Host time: the end of the block (`RenderCore::rendered_until()`). |
   | `output_latency` | Duration: the block's `output_latency`. |
   | `output_state` | Code: `RenderCore::output_state()`. |
   | `flags` | Bit 0: muted. Bit 1: silenced. |
   | `non_finite_samples` | `RenderCore::non_finite_samples()`. |
   | `limited_frames` | `RenderCore::limited_frames()`. |
   | `late_blocks` | `RenderCore::late_blocks()`. |
   | `render_time` | Duration: how long the block took to render (requirement 12). |
   | `render_load` | Real: `RenderCore::render_load()`. |
   | `peak_render_load` | Real: `RenderCore::peak_render_load()`. |
   | `clients` | `kStateClients` `ClientAck`s, slot 0 first. |

7. **`EngineState`**, the engine-state segment's layout after its header: `kStateDecks` `Seqlock<DeckState>`s (`decks`), then a `Seqlock<MixerState>` (`mixer`), then a `Seqlock<EngineStatus>` (`status`). `kEngineStateLayoutVersion` is 1. `kEngineStateSegmentSize` is `kSegmentHeaderSize + sizeof(EngineState)`, and an `EngineState` lives at offset `kSegmentHeaderSize`.
8. Every struct in this header is standard-layout and trivially copyable, with `static_assert`s on its size and its fields' offsets. Zero-filled memory is a valid `EngineState` in which nothing has been published.

### Tracks, blocks, and drivers

9. **Track ID:** `Track`'s constructor takes an optional `std::uint64_t id` after the sample rate, 0 by default, and `Track::id()` returns it.
10. **Output latency:** `Block` gains `std::chrono::nanoseconds output_latency`, 0 by default, after `host_time`: how long after a frame is due at the driver it's heard. Drivers set it; the render core passes it to the graph unchanged.
11. **`render_interleaved()`** takes an optional `output_latency` after `driver`, 0 by default, and gives it to every block it renders. The paced, offline, and miniaudio drivers pass 0 for now. Estimating a device's latency comes later (out of scope).

### The render core (`<ks/engine/render_core.h>`)

12. **Render timing:** for each block it renders, the render core reads the monotonic clock with `ks::core::monotonic_now()` once the driver check has passed, and again after the safety stage. The difference is the block's render time. Its render load is the render time divided by the block's duration (`frames_to_duration()`). Skipped and rejected blocks aren't timed.
13. **Load and late blocks**, readable from any thread:
    - `blocks_rendered()`: blocks rendered;
    - `late_blocks()`: rendered blocks whose render time was longer than their duration;
    - `render_time()`: the last rendered block's render time, 0 before the first;
    - `render_load()`: the last rendered block's load, 0 before the first;
    - `peak_render_load()`: the highest load of any rendered block, 0 before the first;
    - `muted()`: whether `set_muted()` last muted the output.
14. **Output state:** `OutputState` moves from `<ks/engine/output_controller.h>` to `<ks/engine/render_core.h>`, unchanged. `RenderCore::set_output_state()` and `output_state()` store and load it from any thread; it's `Stopped` until set. `OutputController` sets it every time its own state changes, so the core's state always equals `OutputController::state()` once a controller call returns.
15. **The rendered hook:** `Graph` gains a virtual `rendered(const Block& block, const RenderStats& stats) noexcept KS_NONBLOCKING`, which does nothing by default.
    - The render core calls it once for each block it renders, after the safety stage and after updating every count, while still holding the render token. It never calls it for a skipped or rejected block.
    - `block` is the block the graph rendered (with `resumed` as the core set it). `stats` holds the core's values after this block: blocks and frames rendered, NaN and infinite samples, limited frames, late blocks, this block's render time and load, the peak load, the output state, and whether it's muted and silenced. Each equals the matching accessor.
    - The hook's own time isn't part of the block's render time.

### The engine (`<ks/engine/engine.h>`)

16. **The state block:** `Engine` gains a constructor `Engine(ipc::EngineState& state, const BusMap& map = {})`. The state block must outlive the engine. The engine writes it only from the audio thread, in `rendered()`, and never reads it except as `SeqlockWriter` does when it's constructed. An engine built without one publishes nothing.
17. **Publishing:** in `rendered()`, the engine writes each deck's block, then the mixer's, then the status block, each once, from what it holds after the block (requirements 3 to 6). Blocks the render core doesn't render publish nothing, and nothing is written before the first rendered block.
18. **Publishing changes nothing else:** with or without a state block, the engine renders the same output, bit for bit, and its other behavior (6a) is unchanged.
19. **Readers:** any number of threads, later processes, read the blocks with `SeqlockReader` while the engine renders. Every successful read is one block's state as written; the engine never waits for a reader.
20. **Real time:** everything `rendered()` and `try_render()` call is `KS_NONBLOCKING`, allocates nothing, and takes no lock.

## Acceptance criteria

The tests are in `tests/acceptance/state-out/`. They render through a `RenderCore` at 48 kHz, in blocks of 64 frames on 4 zero-filled channels, unless a criterion says otherwise. The state block is a value-initialized `ipc::EngineState` on the heap, unless a criterion says otherwise. Reads use `SeqlockReader`.

The layout (`layout_test.cpp`):

- [ ] `[auto]` `DeckState`, `MixerState`, `ClientAck`, and `EngineStatus` have the fields of requirements 3 to 6, in order, at 8-byte steps from offset 0; every struct in the header is standard-layout and trivially copyable; `kStateDecks` and `kStateClients` equal `engine::kDecks` and `engine::kMaxClients`; `kEngineStateLayoutVersion` is 1; and a `Region` of `kEngineStateSegmentSize` bytes holds an `EngineState` at `kSegmentHeaderSize`. (test: "State: the layout")

Tracks, blocks, and the render core (`render_core_test.cpp`):

- [ ] `[auto]` **Track ID:** a track built with ID 42 reports 42; one built without an ID reports 0. (test: "Track: an ID")
- [ ] `[auto]` **Output latency:** `render_interleaved()` with a latency of 3 ms over 1,000 frames gives every block 3 ms, and without one, 0. (test: "Render interleaved: the output latency reaches every block")
- [ ] `[auto]` **The hook:** a recording graph's `rendered()` is called once per rendered block and never for a skipped or rejected one; it gets the block it rendered, including its output latency; and every field of its stats equals the matching accessor read right after `try_render()` returns. (test: "Render core: the graph hears about each rendered block")
- [ ] `[auto]` **Load:**
  - a graph that does nothing, in 512-frame blocks, makes no block late, and its load is below 1;
  - a graph that spins for twice its block's duration makes every block late, with a load of at least 2;
  - `peak_render_load()` is at least every load seen, and `blocks_rendered()` counts every rendered block.

  (test: "Render core: load and late blocks")
- [ ] `[auto]` **Output state:** the core reports `Stopped` until set, and what was set after. With the paced simulated device of the device-loss tests, the core's output state follows the controller through playing, freewheeling after an unplug, playing after the device returns, and stopped. (test: "Render core: the output state follows the controller")

The engine (`engine_state_test.cpp`):

- [ ] `[auto]` **Each block publishes once:** after N rendered blocks, every seqlock's sequence number is 2N and every block's heartbeat is N. Skipped blocks (another driver active) and rejected blocks (0 frames) change nothing. Before the first block, everything reads as zero. (test: "State: each rendered block publishes everything once")
- [ ] `[auto]` **Decks:** through loads (tracks with IDs), play, seek, rate changes, a cue point, an unload, and a track still decoding (waiting at its limit, then complete), each deck's block equals, after every block, what requirement 3 says, read from the engine's decks: the flags, the track's ID, sample rate, capacity, and decoded frames, and the position, rate, and cue, bit for bit. An empty deck has flags 0 and no track fields. (test: "State: decks")
- [ ] `[auto]` **Visual play position:** with blocks at synthetic host times and an output latency of 5 ms, each deck's `host_time` is the block's end and `output_latency` is 5 ms. A deck playing at rate 1.0 a 48 kHz track advances `position` by 64 frames per block once its rate has settled, and each `host_time` is 64 frames after the last. (test: "State: the visual play position")
- [ ] `[auto]` **Mixer:** after commands set every control, including −∞ trim and levels and a new bus map, the mixer block equals `settings()`, field by field and bit for bit. (test: "State: the mixer")
- [ ] `[auto]` **Status:**
  - the heartbeat, frames rendered, sample rate, host time, and output latency match the render core and the block;
  - a track with NaN samples raises `non_finite_samples`, and a loud track with +12 dB trim raises `limited_frames`, as the render core counts them;
  - muting sets the muted flag, and then the silenced flag once the fade ends;
  - a set output state appears in the next block;
  - the late-block count, render time, load, and peak load match the render core's accessors.

  (test: "State: status")
- [ ] `[auto]` **Acknowledgements:** with three clients attached, sending valid and invalid commands, each slot's `ClientAck` equals `client_stats()` after every block, including the last rejection's code; a detached slot reads as not attached; a corrupt ring's slot has its corrupt flag. (test: "State: acknowledgements")
- [ ] `[auto]` **Publishing changes nothing:** a script that loads two tracks, plays them, and sends crossfader, fader, rate, and seek commands at scheduled frames renders bit-identical output with and without a state block. (test: "State: publishing doesn't change the output")
- [ ] `[auto]` **Under load:** one thread renders 64-frame blocks without pause for 2 seconds, while three threads read every block as fast as they can. Every successful read is consistent: the status's `frames_rendered` is 64 times its heartbeat; a deck that has played a 48 kHz track at rate 1.0 since the first block has a `position` equal to the smaller of 64 times its block's heartbeat and the track's length; heartbeats never go backwards for any reader. Each reader succeeds at least 1,000 times. Under the `tsan` preset, ThreadSanitizer reports nothing. (test: "State: readers never see a torn block")
- [ ] `[auto]` **Real time:** rendering an engine with a state block, through `try_render()`, with commands and loads, compiles under function-effect analysis and is clean under `rtsan`. (test: "State out is real-time safe")
- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux and on macOS arm64 (`debug`, `release`, `asan`, `tsan`).

## Out of scope

- Smoothing host times with a delay-locked loop, and estimating a device's own latency (Phase 3, with the waveform view). The miniaudio driver's latency estimate (step 2c).
- Meters (Phase 2, with the UI).
- The resume state (6c).
- Device underruns as the device reports them (step 2c, where a backend offers them).
- Typed client-side readers that validate the blocks (1b, with the first client process).
- Creating the segment and passing it between processes (1b).

## Open questions

None.
