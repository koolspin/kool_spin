# Feature: ks-tone options

- **Status:** Approved by the maintainer on 2026-11-09. Protected: build loops can't change it.
- **Roadmap:** Phase 0, step 6 (the trial loop)
- **Built by:** loop

## Goal

Let `ks-tone` play a tone at a chosen frequency, level, and length, on a chosen output device, so audio hardware such as the Inpulse 500 can be tested without changing the system's default device. This is also the first feature built by a build loop, to try the workflow end to end.

## Context

- `ks-tone` lives in `src/apps/tone/`:
  - **`main.cpp`:** argument handling and output.
  - **`ks::tone::play_tone()`:** the miniaudio glue, in `tone_player.cpp`.
  - **`ToneGenerator`:** renders the tone on the audio thread with 10 ms fades, in `tone_generator.cpp`.
- Today it takes only `--backend` and `--help`, and plays 440 Hz at -20 dBFS for 2 s on the default device.
- The real-time rules in `CLAUDE.md` apply to everything that runs on the audio thread: `ToneGenerator::render()` and the miniaudio callback.
- miniaudio's null backend (`--backend null`) runs a real audio thread without hardware, at 48 kHz. Its only output device is `NULL Playback Device`, which is also its default.

## Requirements

### Options

1. `--frequency HZ` sets the tone's frequency: a number from 20 to 20000. Default 440.
2. `--level DBFS` sets the tone's peak level: a number from -60 to -6. Default -20. The upper limit is a safety cap.
3. `--duration SECONDS` sets how long the tone plays before it fades out: a number from 0.1 to 60. Default 2.
4. Numbers are decimals such as `440`, `440.5`, or `-12`. Anything else is invalid, including `nan`, `inf`, an empty string, and trailing characters (`440x`).
5. `--list-devices` lists the output devices of the selected backend and exits without playing. When `--backend` isn't given, it lists the backend that playback would use. Other options are ignored.
6. `--device N` plays on device `N` from that backend's `--list-devices` output instead of the default device. `N` is a whole number, 0 or greater.
7. `--help` documents every option, with its range and default.

### Errors

Usage errors print one line to stderr and exit with code 2, without playing anything. Runtime failures, such as a backend or device that won't open, exit with code 1, as today.

8. A missing value: `ks-tone: --<option> needs a value`.
9. An invalid or out-of-range number: `ks-tone: --<option> must be a number from <min> to <max>`. For example: `ks-tone: --level must be a number from -60 to -6`.
10. An invalid `--device` value: `ks-tone: --device must be a device number shown by --list-devices`.
11. A device number that doesn't exist: `ks-tone: no output device <N> on <backend>`, where `<backend>` is miniaudio's name for it, such as `Null`.
12. A frequency at or above half the device's sample rate: `ks-tone: --frequency <F> Hz is too high for the device's <R> Hz sample rate`. This can only be checked once the device is open.

### Output

13. Playback prints:

    ```
    Playing on <backend>: <device>
      <rate> Hz, <channels> channels, <period> frames x <periods> periods (<ms> ms)
      <frequency> Hz at <level> dBFS for <duration> s
    Done: <frames> frames, peak <peak> dBFS, <measured> Hz
    ```

    - The first two lines are unchanged from today.
    - In the third line, the settings are printed in their shortest form: `440`, `440.5`, `-12`, `0.5`.
    - `<peak>` is the peak level actually rendered, with one decimal place, as today.
    - `<measured>` is the tone's frequency measured from the rendered samples, rounded to the nearest whole Hz. It must be within 0.1% of the requested frequency.
14. `--list-devices` prints:

    ```
    Output devices on <backend>:
      <N>: <name>
    ```

    - One line per device, numbered from 0, in miniaudio's order.
    - The system default device has ` (default)` after its name.

### Unchanged

15. The real-time rules apply. The audio thread doesn't allocate, lock, or call miniaudio, and the frequency measurement runs on the audio thread under the same rules.
16. Existing behavior and tests keep working, except where this spec changes them.

## Acceptance criteria

The `[auto]` criteria are checked by `tests/acceptance/ks-tone-options/`. Those tests run `ks-tone` on the null backend.

- [ ] `[auto]` Defaults: 440 Hz at -20 dBFS for 2 s (test `ks-tone-options.defaults`)
- [ ] `[auto]` Frequency, level, and duration are applied, verified by the measured frequency and peak (tests `.options`, `.lowest`, `.highest`, `.decimals`)
- [ ] `[auto]` `--list-devices` lists the null backend's device as the default, without playing (test `.list_devices`)
- [ ] `[auto]` `--device 0` plays on it, and a device number that doesn't exist is rejected (tests `.device`, `.device_missing`)
- [ ] `[auto]` Missing, invalid, and out-of-range values are rejected with the specified messages and exit code 2, without playing (tests `.invalid.*`)
- [ ] `[auto]` `--help` documents every option (test `.help`)
- [ ] `[manual]` On Linux, `--list-devices` shows the Inpulse 500 on the `pulseaudio` and `alsa` backends, and `--device N` plays through it
- [ ] `[manual]` The same on macOS with the `coreaudio` backend

Requirement 12 can't be tested end to end: the null backend always runs at 48 kHz, so no frequency in range reaches its limit. The build must cover it with unit tests.

## Out of scope

- Choosing output channels, for example the Inpulse's headphone outputs 3-4
- Choosing the sample rate or buffer size
- Input devices
- Playing until a key is pressed
- Any change to `--backend`

## Open questions

None.
