# ks-tone options

## Summary

`ks-tone` now takes `--frequency HZ`, `--level DBFS`, `--duration SECONDS`, `--list-devices`, and `--device N`, so audio hardware such as the Inpulse 500 can be tested without changing the system's default output device. Playback prints the requested settings and, at the end, the frequency actually measured from the rendered samples. Usage errors are one line on stderr with exit code 2, and runtime failures still exit with code 1. This is the first feature built by a build loop (Phase 0, step 6). It took 8 passes, and no attempt failed.

All 20 acceptance tests pass. The `debug`, `asan`, and `rtsan` workflows pass in full, and `tools/lint.sh` is clean.

## What was built, task by task

1. **Option parser** (`ks/tone/options.h`, `options.cpp`): `parse_options()` returns `Options` or the one-line error message. Numbers get a strict syntax check, then `std::strtod` must consume the whole string, then the result must be finite and in range. Ranges and defaults are named constants that `--help` reuses. Covered by `tests/tone/options_test.cpp`, including the boundaries, `nan`, `inf`, `""`, `440x`, `1e3`, and `+440`.
2. **`main.cpp` uses the parser.** It has a new `--help` with every option's range and default, prints the third playback line (`<F> Hz at <L> dBFS for <D> s`, shortest form), and `play_tone()` takes `std::chrono::duration<double>`.
3. **`ks::dsp::FrequencyMeter`** (header-only): counts upward zero crossings with linear interpolation and measures from the first crossing to the last. It allocates nothing and is `KS_NONBLOCKING`. Tested at 20 to 20000 Hz, lengths from 0.1 to 1 s, at 0 and -60 dBFS, with and without fades, all within 0.05%.
4. **Measuring on the audio thread:** `ToneGenerator::render()` feeds the meter and publishes the result through a lock-free `std::atomic<double>`. `PlaybackReport::measured_hz` carries it, and the `Done:` line prints it rounded to whole Hz.
5. **`--list-devices`:** `list_devices()` shares the backend selection (`open_context()`) with `play_tone()`, so with no `--backend` it lists the backend that playback would use.
6. **`--device N`:** `play_tone()` returns `PlayError { Kind::Usage | Kind::Runtime, message }`. An index past the end is a usage error (`no output device N on <backend>`), reported before any device opens.
7. **Frequency too high for the device (requirement 12):** `check_frequency()` runs right after `ma_device_init` and before the `ToneGenerator` is constructed, whose oscillator asserts the Nyquist limit. A frequency that's too high is a usage error, and nothing plays. Unit tests cover it, plus a library-level `play_tone` case at 30000 Hz.
8. **Finish:** ran the full suite and checked the docs. No fixes were needed.

## How the `[auto]` criteria are covered

| Criterion | Tests |
|---|---|
| Defaults | `ks-tone-options.defaults` |
| Frequency, level, and duration are applied | `.options`, `.lowest`, `.highest`, `.decimals` |
| `--list-devices` | `.list_devices` |
| `--device 0` plays on it, a missing device is rejected | `.device`, `.device_missing` |
| Missing, invalid, and out-of-range values | `.invalid.*` |
| `--help` | `.help` |
| Requirement 12 (no end-to-end test possible) | `check_frequency` unit tests and the 30000 Hz `play_tone` case in `tests/tone/tone_player_test.cpp` |

## Deviations and decisions

None from the spec. Decisions recorded in `plan.md`:

- **Requirement 12 exits with code 2.** It's a usage error, like requirement 11, even though it's found only after the device opens.
- **`--list-devices` still parses the other options.** A malformed value is still a usage error, but valid values don't affect the listing.
- **`--backend` is unchanged.** An unknown backend still exits with code 1 and today's message.
- **Number syntax:** the parser uses a hand-written syntax check plus `std::strtod`, not floating-point `std::from_chars`, which macOS's libc++ (AppleClang 17) lacks. `.5` and `0.50` are accepted.
- **Unknown arguments** keep `ks-tone: unexpected argument '<arg>'` but now print only that line, with no usage dump after it.
- **`--backend` with no value** now gives `ks-tone: --backend needs a value` (requirement 8, applied uniformly).
- **The value after an option is always taken as its value**, so `--frequency --level` reports `--frequency` as invalid.
- **`play_tone()`'s signature changed:** the error type is now `PlayError`, and `device_index` is the fourth parameter, before `on_start`.

## Learnings worth adding to `CLAUDE.md`

- **The loop sandbox:** Catch2 test discovery needs a writable `XDG_RUNTIME_DIR`. Run `cmake -E env XDG_RUNTIME_DIR=<worktree>/build ctest ...`. `&&`, pipes inside some commands, `$PWD`, and `python3` heredocs were refused, so run configure, build, and test as separate commands and edit files with the Edit tool. Worth putting in `docs/process.md` or the loop prompt.
- **`std::format("{}", double)`** picks the shorter of fixed and scientific notation, so values like 1e6 print as `1e+06`. That's fine for the 20 to 20000 Hz range, but watch for it when printing larger numbers.
- **clang-tidy rules:** it requires enumerators in `CamelCase` and, through `performance-enum-size`, a narrow underlying type such as `std::uint8_t`.
- **Include order:** lint puts the header a test covers first, in its own block.

## `[manual]` acceptance criteria to check by hand

- [ ] On Linux, `ks-tone --backend pulseaudio --list-devices` and `ks-tone --backend alsa --list-devices` show the Inpulse 500, and `ks-tone --backend <b> --device N` plays through it. The `Done:` line should report about 440 Hz.
- [ ] On macOS, the same with `--backend coreaudio`.
- [ ] Optional: if the Inpulse runs at 44.1 kHz, `--frequency 20000` plays, and on a device at 32 kHz or lower it's rejected with exit code 2 (requirement 12).
