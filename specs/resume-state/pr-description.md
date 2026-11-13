# Resume state (Phase 1a, step 6c)

## Summary

The engine can now continue where a crashed engine stopped. After every block it renders, it writes a snapshot of each deck (track ID, play state, position, rate, cue point), the mixer's settings, and the block's end time. Snapshots alternate between two seqlocked slots in `EngineState::resume`, and each carries a generation number and a CRC-32. A new engine built with `Engine(state, tracks, map)` reads the newest slot that passes every check. It reloads each deck's track by ID, restores the mixer, and moves playing decks on by the time that has passed. It then fades the output in over 50 ms. If neither slot is valid, it starts clean, with decks paused and the bus map it was given.

All 18 resume-state acceptance tests pass. So does every test in `debug`, `release`, `asan`, `tsan`, `rtsan`, and `fuzz` on Linux (`ks_resume_state_fuzz` runs 200,000 inputs in about 63 s), and `tools/lint.sh` is clean.

## What was built, task by task

The CRC-32 and the resume slots in `<ks/ipc/state.h>` were done before the loop started (5305cf0).

1. **`encode()`** (`src/engine/resume.cpp`): fills every `ResumeState` word and appends the CRC of the words before it. The encoders that `rendered()` already used (`real_code`, `host_time_code`, `side_code`, `flag_code`) moved to a new internal header, `src/engine/codes.h`, so mixer fields are encoded the same way in `MixerState` and `ResumeState`. Unit test: `tests/engine/resume_test.cpp`.
2. **`validate()`**: copies the words into a local array first, then checks slot/generation parity, the generation range, the checksum's high 32 bits and CRC, host time ≥ 0, flags, enum codes, and every range in requirement 7. Range checks are written so that NaN fails them. Decoding is a bit-exact `std::bit_cast`.
3. **`read_snapshot()`**: one `try_read()` per slot, then validation, keeping the higher generation.
4. **Writing snapshots** (`Engine::write_snapshot()`, called at the end of `rendered()`): generation `g` goes to slot `g % 2`. Generations continue from the snapshot found at construction, or start at 1. Either constructor calls `read_snapshot()` once, and the result is kept in `found_snapshot_`.
5. **The resuming constructor**: delegates to `Engine(state, map)`. It then applies the mixer settings through the existing setters, loads the first non-null track whose ID matches, sets the cue point, records the reference in the deck's first place (the same bookkeeping as `apply_load()`), and sets the rate.
6. **The first block's moves** (`Engine::resume_moves()`): on the first rendered block, it seeks each resumed deck to `position + rate × track rate × elapsed` (elapsed is clamped at 0) and starts the decks that were playing. `Deck` didn't need any changes: a deck seeked to an end and set playing stops there on its first frame.
7. **The fade-in** (`Engine::apply_fade()`): after `mixer_.process()`, it scales each sample of frame `n` by `n / N` (computed as a `double`) for `n < N`. The count carries across blocks, so the output is the same bit for bit at any block size.
8. **Finish**: ran every workflow and lint, and checked the diff for macOS portability. No code changes were needed.

## Deviations from the spec or plan

- **How the fade length is rounded (task 7):** `N` is computed in integers, `(50 × rate + 500) / 1000`, using `kResumeFade`'s count and period, instead of `llround()` on a `double`. This is exact rounding to the nearest frame, with ties rounding up as `llround` does. It also avoids a libm call inside function-effect analysis. It gives the 2,400 and 2,205 frames the tests expect. A sample rate too low to fit one frame in 50 ms (`N = 0`) gets no fade.
- **Generation saturation (task 4):** the generation stops increasing at `kMaxGeneration` instead of wrapping past it, both when continuing from a found snapshot and when incrementing. The plan allowed this. In practice the limit is unreachable.
- **Bus-map check (task 2):** `Mixer::valid(BusMap)` is private, so `resume.cpp` repeats the same conditions inline: both entries even, both below `kMaxOutputChannels`, and different.

## Learnings worth adding to `CLAUDE.md`

- clang-tidy rejects indexing a `std::array` with a non-constant index. The code here uses `std::span{array}[i]`, or a range-for with a counter. This is worth stating as a convention.
- Inside `KS_NONBLOCKING` code, prefer exact integer arithmetic to libm functions (`llround`, etc.), which function-effect analysis may not accept.
- Range checks on values read from shared memory should be written as `x >= lo && x <= hi`, so that NaN fails them. A negated comparison lets NaN through.

## `[manual]` acceptance criteria

The spec has no `[manual]` criteria. One `[auto]` criterion, **Every CI configuration**, can only be checked by CI: `debug`, `release`, `asan`, and `tsan` on macOS arm64. The loop builds only on Linux, so confirm the macOS jobs pass on this pull request.
