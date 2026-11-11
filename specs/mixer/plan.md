# Plan: mixer

Written by the planning pass from `spec.md`. Each build pass takes the first unchecked task, ticks it when every check passes, and adds a note beneath it. A failed attempt is recorded as a note; three failed attempts on one task mean the loop writes `BLOCKED.md`.

Files involved: `src/engine/mixer.cpp` (the stubs), `src/engine/include/ks/engine/mixer.h` (private members only; the public API must not change), a new `tests/engine/mixer_test.cpp`, and `tests/CMakeLists.txt` (not protected; add the new file to `ks_engine_tests`). Read but don't edit: `tests/acceptance/mixer/` (`mixer_test.cpp`, `realtime_test.cpp`, `test_support.h`, `listen_test.cpp`) and `tests/acceptance/deck/test_support.h`, all protected. Useful models: `Deck::set_sample_rate()` in `src/engine/deck.cpp` (smoothing coefficient with `-std::expm1`) and `tests/engine/deck_test.cpp` (building tracks from sample vectors in unit tests).

State before the loop: the stubs compile, and all 8 mixer acceptance tests fail (per the commit that added them). The golden-file criterion is out of the loop's scope (spec), and the listening test is hidden and `[manual]`.

## Tasks

- [ ] 1. **Controls, validation, and `settings()`.** Implement every setter in `mixer.cpp` so it updates `settings_` only (no audio yet):
  - per-deck controls return early when `deck >= kDecks`; `double` controls return early on NaN (`std::isnan`);
  - trim: `std::min(db, kMaxTrimDb)` (−∞ is kept); fader, crossfader, cue mix: `std::clamp(v, 0.0, 1.0)`; transition: `std::clamp(v, kMinCrossfaderTransition, 1.0)`; main and headphone levels: `std::min(db, 0.0)` (−∞ kept);
  - `set_crossfader_assign` stores the side as is (an out-of-range enum value can't be produced by the API; if clang-tidy or the reviewer wants it, treat anything other than `A`/`B` as `Thru`);
  - a private `static bool valid(const BusMap&) noexcept KS_NONBLOCKING`: both even, both `< kMaxOutputChannels`, different. The constructor stores `map` if valid, else `BusMap{}`; `set_bus_map()` ignores an invalid map.

  Add `tests/engine/mixer_test.cpp` to `ks_engine_tests`: defaults equal `MixerSettings{}`; each clamp at and past its bounds (including +∞ and −∞ for each `double` control: +∞ trim becomes 12, −∞ fader becomes 0, and so on); NaN and deck index 4 change nothing; every valid and invalid bus map shape (odd, equal, 8, `{4, 6}`, `{6, 4}`) through both the constructor and `set_bus_map()`. (passes: "Mixer: controls are validated"; advances: "Mixer: buses go to their channel pairs" (its invalid-maps section))

- [ ] 2. **Mixing and routing, with gains jumping straight to their targets.** Add private state to `Mixer`:
  - deck buffers: `std::array<std::array<float, kMaxBlockFrames>, kDecks * 2>` (or left/right arrays per deck), members of the mixer so nothing allocates;
  - the gains: aᵢ and cᵢ for each deck, plus m, h_c, h_m, each a `double` (a small private struct `Gains` with `std::array<double, kDecks> channel, cue; double main, headphone_cue, headphone_main;` keeps tasks 2 and 3 tidy).

  A private `Gains targets() const noexcept KS_NONBLOCKING` computes requirement 9 from `settings_`: trim gain `std::pow(10.0, db / 20.0)` (−∞ gives exactly 0), fader², crossfader gain from requirement 4 (side A: 1 while x ≤ 1 − w, else `std::cos(π/2 · (x − (1 − w)) / w)`; side B: side A at 1 − x; `Thru`: 1), cue 1 or 0, m and h from dB the same way, h_c = h · (1 − cue mix), h_m = h · cue mix. Write it in the same operation order as `side_a()` in the acceptance `test_support.h` so results agree closely. The constructor sets the gains to `targets()` (requirement 10, at construction).

  `process(block)`:
  - return without writing if the block is invalid: `frames` is 0 or above `kMaxBlockFrames`, `sample_rate` is 0, or (for the channels a bus would use) a channel span is shorter than `frames`;
  - read `settings_.bus_map` once into a local, so a map change applies from the next block;
  - render each deck into its buffers (`deck.render(left.first(frames), right.first(frames), sample_rate)`); the deck writes every frame, paused or not;
  - in this task, set the gains to `targets()` once at the top of the block (task 3 replaces this with per-sample smoothing);
  - per frame, accumulate in `double`: main L/R = Σ aᵢ · deckᵢ, cue L/R = Σ cᵢ · deckᵢ; main out = m · main; headphones = h_c · cue + h_m · main. Cast to `float` once on writing;
  - write the main pair only if `map.main + 1 < block.channels.size()`, and the cue pair likewise; write no other channel.

  Unit tests in `tests/engine/mixer_test.cpp`, with small constant tracks: an 8-channel block with map `{4, 6}` writes only channels 4–7 and leaves 0–3 untouched (prefill them with a sentinel to prove nothing is written); a 3-channel block with the default map writes only 0 and 1; an invalid block writes nothing; paused decks give silence; a −∞ main level silences main but not the headphones. (passes: "Mixer: buses go to their channel pairs", "Mixer: trim and fader", "Mixer: crossfader curves", "Mixer: cue bus and headphone mix", "Mixer is real-time safe")

- [ ] 3. **Per-sample gain smoothing.** Replace the block-start jump with requirement 10:
  - a private `double smoothing_` and `std::uint32_t sample_rate_ = 0`; when the block's rate differs, `smoothing_ = -std::expm1(-1.0 / (std::chrono::duration<double>(kGainSmoothing).count() * rate))`, exactly as the acceptance smoothing test computes it (0.001 · 48000 = 48 either way);
  - compute `targets()` once per block (controls only change between blocks), then for each frame, before mixing it: for each of the 11 gains, `gain += (target − gain) · smoothing_`, then `if (std::abs(gain − target) <= 1e-6) gain = target;`;
  - the state is carried across blocks, so the output doesn't depend on block size (requirement 11), and a settled gain equals its target exactly, so settled output is bit-identical to a mixer that started there.

  Unit tests: the per-frame gain sequence after a fader move matches a `double` reference, across block sizes 1, 37, and 64; a gain within 10⁻⁶ snaps to its target; a sample-rate change between blocks recomputes the coefficient and keeps the current gains; a cue toggle and a bus map change in the same script don't break block-size invariance. (passes: "Mixer: gain changes are smoothed", "Mixer: output doesn't depend on block size")

- [ ] 4. **Finish.** Run the complete suite in `debug`, `asan`, and `rtsan` (`cmake --workflow --preset <preset>`), including all acceptance tests, and `tools/lint.sh`. Fix anything outstanding. Update the stub comment at the top of `mixer.cpp` (it says the mixer ignores its controls) and check that the doc comments in `mixer.h` (private members only) describe the implementation. If `ScriptedMixer` / the hidden listening test in `listen_test.cpp` builds, leave it alone; it's `[manual]`. Write `specs/mixer/pr-description.md`: what changed, how each `[auto]` criterion is covered, the `[manual]` listening criterion and the golden-file test for the maintainer to add after listening, the macOS CI criterion (only CI can check it), the Decisions below, and Learnings. (passes: everything the loop can pass)

## Decisions

- **Order of tasks.** Validation first (pure state, one acceptance test passes at once). Mixing next with gains that jump to their targets: every steady-state acceptance test waits 2,000 frames and checks the last frame, so five tests pass before smoothing exists. Smoothing last, since only the smoothing and block-size tests depend on it.
- **When targets are computed.** Once per block, from `settings_`. Controls are only called between blocks, so this is the same as recomputing in every setter, and it keeps `std::pow`/`std::cos` out of the per-frame loop and the setters trivial. `settings()` reports what was set, and the smoothed gains live separately (requirement 6).
- **Smoothing coefficient.** `-std::expm1(-1 / (τ · rate))` rather than `1 − std::exp(...)`: the same value mathematically, more precise, and exactly what the acceptance smoothing test and `Deck` use.
- **Snapping.** `|gain − target| <= 1e-6` (inclusive), matching the acceptance test's reference. Snapping is what makes the settled output bit-identical to a fresh mixer.
- **Mixing precision.** Gains and the sums are `double`; each output sample is cast to `float` once. This keeps the 10⁻⁶ checks comfortable and the arithmetic identical regardless of block size.
- **Invalid blocks.** The render core only passes valid blocks, but `process()` is a public entry point, so it checks the block like `Deck::render()` does and writes nothing if it's invalid, rather than indexing out of range.
- **Which channels a bus needs.** A bus is written when both `map.x` and `map.x + 1` are below `block.channels.size()` (requirement 2, "both of its channels exist in the block").
- **Math in `KS_NONBLOCKING` code.** `std::pow` and `std::expm1` are already called from nonblocking engine code (`safety.cpp`, `deck.cpp`), so they pass function-effect analysis. `std::cos` is expected to behave the same; the commit that added the acceptance tests notes that a throwaway implementation passed all 8. If the analysis rejects it, record that in the plan rather than suppressing a diagnostic (suppressions need the maintainer).

## Learnings

- From the ks-tone-options loop: in the loop sandbox, run `ctest` as `cmake -E env XDG_RUNTIME_DIR=<worktree>/build ctest --preset <preset> ...` (Catch2 discovery needs a writable runtime dir), pass the worktree path literally (no `$PWD`), and run configure, build, and test as separate commands (no `&&` or pipes). Edit files with the Edit tool.
- Code must compile on macOS with AppleClang 17 (see `CLAUDE.md`); nothing in this plan needs a missing libc++ feature.
