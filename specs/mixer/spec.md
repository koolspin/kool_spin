# Feature: Mixer

- **Status:** Approved by the maintainer on 2026-11-11. Protected: build loops can't change it.
- **Roadmap:** Phase 1a
- **Built by:** loop

## Goal

Mix four decks the way a DJ mixer does: a trim, a channel fader, a crossfader assignment, and a cue button on each channel, a crossfader, and a main bus and a headphone bus, each sent to a pair of the device's channels. Every gain change is smoothed, so nothing clicks or zippers.

## Context

- **Decisions that apply:**
  - ADR 0003 §3: block-size invariance. Smoothing works per sample and carries its state across blocks.
  - ADR 0003 §10: everything the audio thread uses is allocated before it runs.
  - ADR 0003 §15: in Phase 1, the main and headphone buses go to channels of one device. The Inpulse 500 has outputs 1–2 and 3–4.
  - The real-time rules in `CLAUDE.md`.
- **The maintainer's design choices** (2026-11-11):
  - a standard DJ control set;
  - fader and crossfader curves in the engine, with controls carrying positions;
  - the safety stage limits each bus separately;
  - no built-in headroom: a main level control, 0 dB by default, and the safety limiter catches overs.
- **What exists:**
  - `Deck` (`<ks/engine/deck.h>`) plays one track into two channels. `Interpolator` is shared by every deck.
  - `Graph` is what the render core runs. The mixer is a graph.
  - **The safety stage already limits each stereo pair of channels separately.** That change was made interactively before this loop, together with the amendment to the safety-stage spec. The main and headphone buses therefore never turn each other down.
- **How the mixer is driven, for now:** by calling its methods on the audio thread between blocks, like the deck. The command decoder (step 6) will call the same methods.
- **The API exists before the code:** `src/engine/include/ks/engine/mixer.h` declares everything below, with stubs in `src/engine/mixer.cpp`. The acceptance tests compile against it and fail until the mixer is built. The loop implements the stubs. It may add private members and helpers, but must not change the public API.

## Requirements

### Structure

1. `<ks/engine/mixer.h>` holds `Mixer`, a `Graph`, and `kDecks` = 4.
   - **Decks:** the mixer owns `kDecks` decks, all using the `Interpolator` it's constructed with. `deck(i)` returns deck i, for `i < kDecks`. Channel i of the mixer is deck i.
   - **Memory:** everything is allocated by the time the constructor returns. Rendering and every control allocate nothing.
   - **Threads:** controls are called on the audio thread between blocks, and take effect from the next frame. Every control and `process()` is `noexcept KS_NONBLOCKING`.
2. **The bus map.** `BusMap` names the first of the two device channels each bus goes to: `main` (0 by default) and `cue` (the headphone bus, 2 by default).
   - **Valid maps:** both channels are even, below `kMaxOutputChannels`, and different. The constructor uses the default for an invalid map, and `set_bus_map()` ignores one.
   - **Output:** in each block, a bus is written only if both of its channels exist in the block. The mixer writes nothing else, so other channels keep the silence the render core put there. On a 2-channel device, only the main bus is heard.

### Controls

Every control that takes a deck index ignores an index of `kDecks` or more. Every control that takes a `double` ignores NaN, and clamps other values as stated.

3. **Per channel:**

   | Control | Values | Default | Gain |
   |---|---|---|---|
   | `set_trim(deck, db)` | dB, at most +12; −∞ is allowed | 0 dB | 10^(dB/20) |
   | `set_fader(deck, position)` | [0, 1] | 1 | position² |
   | `set_crossfader_assign(deck, side)` | `A`, `B`, or `Thru` | `Thru` | requirement 4 |
   | `set_cue(deck, on)` | on or off | off | 1 or 0 |

4. **The crossfader.**
   - **Controls:** `set_crossfader(position)` takes [0, 1], where 0 is all side A, and defaults to 0.5. `set_crossfader_curve(transition)` takes [0.01, 1] and defaults to 1.
   - **Gains:** with position x and transition w, side A's gain is 1 while x ≤ 1 − w, and cos((π/2) · (x − (1 − w)) / w) above that. Side B's gain is side A's at 1 − x. A `Thru` channel's crossfader gain is 1.
   - **The curve:** a transition of 1 is a constant-power blend across the whole travel. A small transition is a scratch cut: both sides stay at full level until the last stretch of travel.
5. **Levels:**
   - `set_main_level(db)`: at most 0 dB, −∞ allowed, 0 dB by default.
   - `set_headphone_level(db)`: the same.
   - `set_cue_mix(position)`: [0, 1], 0 by default. 0 is the cue bus only in the headphones, and 1 is the main bus only.
6. **Settings.** `settings()` returns every control's current value as a `MixerSettings`. This is the value set, not the smoothed gains.
7. **The bus map at run time:** `set_bus_map(map)` takes effect from the next block, without smoothing.

### Mixing

8. **Each block:** every deck renders the block's frames at the block's sample rate into the mixer's own buffers. Then, for each frame:
   - **main bus** = Σᵢ aᵢ · deckᵢ, for each channel of the pair;
   - **cue bus** = Σᵢ cᵢ · deckᵢ;
   - **main output** = m · main bus;
   - **headphone output** = h_c · cue bus + h_m · main bus. The headphones hear the main bus before the main level, so turning the main level down for the room doesn't change them.
9. **The gains,** and their targets:
   - aᵢ: trimᵢ · faderᵢ · crossfaderᵢ;
   - cᵢ: trimᵢ · cueᵢ. The cue bus is pre-fader: it ignores the channel fader and the crossfader;
   - m: the main level;
   - h_c: the headphone level · (1 − cue mix);
   - h_m: the headphone level · cue mix.
10. **Smoothing.** Each gain is kept in `double`.
    - **Each frame,** it moves toward its target by (target − gain) · (1 − e^(−1 / (`kGainSmoothing` · sample rate))), where `kGainSmoothing` is 1 ms. Once it's within 10⁻⁶ of the target, it equals the target.
    - **Order:** the gains move first, then the frame is mixed.
    - **At construction,** each gain equals its target.
11. **Block-size invariance.** Given the same controls at the same frames, the mixer's output, like the decks', doesn't depend on how the frames were divided into blocks (ADR 0003 §3).

## Acceptance criteria

The tests are in `tests/acceptance/mixer/`. Unless a criterion says otherwise:
- tests call `process()` directly, at 48 kHz, in blocks of 64 frames, on 4 zero-filled channels;
- each deck plays a 48 kHz track of constants: deck i's left channel is 0.05 · (i + 1), and its right is the negative of that;
- checks wait 2,000 frames after a control, so ramps and smoothing have finished;
- "within 10⁻⁶" is an absolute tolerance.

Mixing (`mixer_test.cpp`):

- [ ] `[auto]` **Routing:**
  - with the defaults, channels 0 and 1 carry the sum of the decks (0.5 and −0.5), within 10⁻⁶, and channels 2 and 3 are silent;
  - with a 2-channel block, only the main bus is written;
  - with the map {main 2, cue 0}, the buses swap;
  - invalid maps (odd, equal, or out of range) are refused.

  (test: "Mixer: buses go to their channel pairs")
- [ ] `[auto]` **The channel strip,** one deck at a time:
  - a trim of +6 dB multiplies its output by 10^(6/20);
  - a trim of −∞ silences it;
  - a fader at 0.5 multiplies it by 0.25;
  - a fader at 0 silences it.

  (test: "Mixer: trim and fader")
- [ ] `[auto]` **The crossfader**, with deck 0 on side A, deck 1 on side B, and decks 2 and 3 thru, at transitions 1 and 0.1 and positions 0, 0.25, 0.5, 0.75, 0.95, and 1:
  - each deck's contribution matches requirement 4 within 10⁻⁶;
  - the thru decks are unaffected.

  (test: "Mixer: crossfader curves")
- [ ] `[auto]` **The headphones:**
  - with deck 2 cued and its fader at 0, channels 2 and 3 carry deck 2 alone, at its trim;
  - a cue mix of 1 gives the main bus before the main level, and 0.5 gives half of each;
  - a headphone level of −6 dB scales the headphones only;
  - a main level of −6 dB scales the main output only.

  (test: "Mixer: cue bus and headphone mix")
- [ ] `[auto]` **Smoothing:** after deck 0's fader moves from 1 to 0, each frame matches requirement 10 within 10⁻⁶, computed in `double`. Once it has settled, the output is identical, bit for bit, to that of a mixer whose deck 0 fader was at 0 from the start. (test: "Mixer: gain changes are smoothed")
- [ ] `[auto]` **Validation:**
  - NaN changes nothing, and neither does a deck index of 4;
  - trim 20 dB becomes 12 dB;
  - fader 2 becomes 1;
  - crossfader −1 becomes 0;
  - transition 0 becomes 0.01;
  - main and headphone levels of +6 dB become 0 dB;
  - cue mix 2 becomes 1.

  `settings()` reports each clamped value. (test: "Mixer: controls are validated")
- [ ] `[auto]` **Block-size invariance:** a script that plays two decks, sweeps the crossfader, moves faders and trims, toggles cue, and seeks a deck renders bit-identical output on all 4 channels with blocks of 1, 37, 128, and 512 frames. (test: "Mixer: output doesn't depend on block size")
- [ ] `[auto]` **Golden file, added after the loop.** Two decks play the synthetic deck-test tracks at 44.1 kHz, at rates 1 and 1.06. The script sweeps the crossfader from A to B over 1 s with transition 1, then cuts back and forth with transition 0.05. It's rendered for 3 s through a render core with the transparent safety configuration by the offline driver. The main bus matches `golden/crossfade.wav` (stereo, 32-bit float, 48 kHz) within 10⁻⁵ per sample. `KS_UPDATE_GOLDEN=1` rewrites it, as in the deck tests. (test: "Mixer: the crossfade matches its golden file")
  - The golden file can't exist before the mixer does, and the loop can't write protected files. So this test and its file are added interactively once the loop has finished, after the maintainer has listened to the file. The loop doesn't build toward this criterion.

Real-time safety (`realtime_test.cpp`):

- [ ] `[auto]` Every `Mixer` control, `settings()`, `set_bus_map()`, and `process()` is called from a `KS_NONBLOCKING` function, so function effect analysis checks them at compile time. Under the `rtsan` preset, RealtimeSanitizer checks them at run time in every test that renders. (test: "Mixer is real-time safe")

Everywhere:

- [ ] `[auto]` All of the above pass in every CI configuration: every preset on Linux, and `debug`, `release`, `asan`, and `tsan` on macOS arm64.
- [ ] `[manual]` **Listening.** The hidden test "Listen: the mixer through the default device" plays two tracks through the default device, with the default safety stage: `KS_LISTEN_TRACK_A` and `KS_LISTEN_TRACK_B`, or the synthetic tracks if they're unset. The main bus goes to channels 1–2, and the headphones to 3–4 when the device has them. The script:
  - a 10 s constant-power crossfade;
  - cue toggles, with the cue mix swept from cue to main;
  - fader rides;
  - 4 s of scratch cuts with transition 0.02.

  Through the Inpulse 500:
  - the crossfade and fader rides are smooth, with no zipper noise;
  - the cue is heard only in the headphones;
  - the cuts are crisp and don't click.

## Out of scope

- EQ, filters, and effects.
- Split cue (cue on one ear, main on the other), and a cue bus on a second device.
- Meters and level reporting, and publishing the mixer's state (step 6).
- Auto-gain from loudness analysis (Phase 2).
- More than 4 decks, and mapping decks to channels in other orders.
- Smoothing the bus map, or changing it while audible.

## Open questions

None.
