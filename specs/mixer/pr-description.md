# Mixer

## Summary

Implements `Mixer` (`src/engine/mixer.cpp`) against the approved spec (`specs/mixer/spec.md`) and the API, stubs, and acceptance tests committed before the loop. Four decks mix into a main bus and a headphone bus. Each channel has a trim, a fader, a crossfader assignment, and a cue button. There's a crossfader with an adjustable curve, plus main level, headphone level, and cue mix. All 11 gains are smoothed per sample with a 1 ms time constant, and their state carries across blocks. The public API in `mixer.h` is unchanged; the loop added only private members and helpers.

Checks at the end of the loop (pass 4, Linux): `cmake --workflow --preset debug`, `asan`, and `rtsan` all pass with 189/189 tests (191 under `rtsan`, which adds its guardrails), including all 93 acceptance tests. `tools/lint.sh` is clean.

## What was built, task by task

1. **Controls, validation, and `settings()`** (pass 1). Every setter updates `settings_` only. A deck index of `kDecks` or more is ignored, NaN is ignored, and each value is clamped as the spec says (−∞ is kept for trim and levels). A private static `valid(BusMap)` is used by both the constructor (which falls back to `BusMap{}`) and `set_bus_map()` (which ignores an invalid map). Added `tests/engine/mixer_test.cpp` to `ks_engine_tests` with 6 unit tests covering the defaults, every clamp at and past its bounds including ±∞, NaN, deck index 4, and every valid and invalid map shape.
2. **Mixing and routing** (pass 2). Private `Gains` (aᵢ, cᵢ, m, h_c, h_m in `double`), per-deck `DeckBuffer`s held as members so nothing allocates, and `targets()`, which computes requirement 9 in the same operation order as the acceptance `test_support.h`. `process()`:
   - rejects an invalid block without writing or rendering: 0 frames, more than `kMaxBlockFrames`, sample rate 0, or a bus whose channels both exist but one is shorter than the block;
   - reads the bus map once per block;
   - renders each deck into its buffer;
   - sums in `double` and casts to `float` once per output sample;
   - writes a bus only if both of its channels exist in the block.

   5 unit tests: an 8-channel block with map `{4, 6}`, a 3-channel block, an invalid block, paused decks, and a −∞ main level.
3. **Per-sample gain smoothing** (pass 3). `smoothing_ = -std::expm1(-1 / (τ · rate))`, recomputed when the block's sample rate changes. `targets()` is computed once per block. Each frame, every gain moves first and then the frame is mixed, and a gain within 10⁻⁶ of its target snaps to it. 4 unit tests: a per-frame gain sequence that matches a `double` reference across block sizes 1, 37, and 64; snapping; a sample-rate change that keeps the current gains; and a cue toggle plus a bus map change that keep block-size invariance.
4. **Finish** (pass 4). Ran the complete suite in `debug`, `asan`, and `rtsan`, plus lint, and nothing needed fixing. The stub comment in `mixer.cpp` was already gone, and the private doc comments in `mixer.h` match the implementation.

## How each `[auto]` criterion is covered

| Criterion | Test | Status |
|---|---|---|
| Routing | "Mixer: buses go to their channel pairs" | passes |
| Channel strip | "Mixer: trim and fader" | passes |
| Crossfader | "Mixer: crossfader curves" | passes |
| Headphones | "Mixer: cue bus and headphone mix" | passes |
| Smoothing | "Mixer: gain changes are smoothed" | passes |
| Validation | "Mixer: controls are validated" | passes |
| Block-size invariance | "Mixer: output doesn't depend on block size" | passes |
| Real-time safety | "Mixer is real-time safe" (function-effect analysis at compile time; RealtimeSanitizer under `rtsan`) | passes |
| Golden file | "Mixer: the crossfade matches its golden file" | **not built.** The spec puts it outside the loop, see below |
| macOS CI | `debug`, `release`, `asan`, `tsan` on macOS arm64 | **only CI can check it.** Nothing in the code uses a libc++ feature missing on AppleClang 17 (no floating-point `from_chars`, no `jthread`) |

## For the maintainer

- **`[manual]` Listening.** Run the hidden test "Listen: the mixer through the default device" (`[.][listen]`, in `tests/acceptance/mixer/listen_test.cpp`) through the Inpulse 500, optionally with `KS_LISTEN_TRACK_A` and `KS_LISTEN_TRACK_B` set. Check that:
  - the 10 s crossfade and the fader rides are smooth, with no zipper noise;
  - the cue is heard only in the headphones (outputs 3–4), including while the cue mix sweeps from cue to main;
  - the 4 s of scratch cuts at transition 0.02 are crisp and don't click.

  It builds as part of the acceptance tests; the loop didn't run it, since it needs a device.
- **Golden file.** Once you've listened, add "Mixer: the crossfade matches its golden file" and `golden/crossfade.wav` interactively. Both are protected, so the loop couldn't add them. Generate the file with `KS_UPDATE_GOLDEN=1`, as in the deck tests.

## Deviations from the spec or plan

None from the spec. Choices the plan left open, or that came up during the loop:

- **Crossfader assignment.** `set_crossfader_assign` stores the side as given. The API can't produce an out-of-range enum value, and clang-tidy didn't object. `crossfader_gain()` treats any other value as `Thru`, as a fallback after its `switch`.
- **Invalid blocks.** An invalid block writes nothing and doesn't render the decks either, so their playheads don't advance. The plan only said "return without writing"; not rendering is the more conservative reading.
- **A pass-2 unit test was narrowed in pass 3.** "A main level of -inf silences main but not the headphones" now checks only the settled second half of the main output. A main level set before the first block smooths down from the constructed 0 dB, because at construction each gain equals its default target (requirement 10).
- **Sample-rate change unit test.** `Deck` restarts its play ramp when the sample rate changes (`ramp_frames_` is counted in frames). The mixer's sample-rate test therefore takes the decks' level from a reference rig rather than assuming constant deck output. That's deck behavior and outside this feature, but you may want to decide whether a mid-play rate change should restart the deck's ramp.

## Decisions (from the plan)

- **Targets once per block.** Controls only change between blocks, so computing `targets()` once per block is equivalent to recomputing in every setter. It also keeps `std::pow` and `std::cos` out of the per-frame loop.
- **Smoothing coefficient.** `-std::expm1(-1 / (τ · rate))`, which is what the acceptance test and `Deck` use.
- **Snapping.** Inclusive `<= 1e-6`, so settled output is bit-identical to a mixer that started at the target.
- **Precision.** Gains and sums are `double`, with one `float` cast per output sample.
- **Bus presence.** A bus is written when both `map.x` and `map.x + 1` are below `block.channels.size()`.
- **`std::cos` in nonblocking code.** It passed function-effect analysis and RealtimeSanitizer, as `std::pow` and `std::expm1` already did. No suppressions were added.

## Learnings worth adding to `CLAUDE.md`

- **Splitting blocks at control frames.** The acceptance harness's `perform()` splits blocks at every control's frame. A mixer whose gains jumped at block starts already passed block-size invariance, so that test alone doesn't prove smoothing carries state across blocks. The unit test that compares per-frame gains across block sizes 1, 37, and 64 is what catches that. Future features with smoothed controls should have the same kind of unit test.
- **Smoothing at construction.** With "at construction, each gain equals its target", a control set before the first block is smoothed from the default, not applied instantly. Tests that set a control and then render the first block should check only the settled part, or the spec should say otherwise.
- **Loop sandbox.** In the loop sandbox, `cmake -E env XDG_RUNTIME_DIR=<worktree>/build cmake --workflow --preset <preset>` works for the full workflow, not only for `ctest`.
