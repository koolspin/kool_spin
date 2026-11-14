# Harness: drive the engine from a script (Phase 1a, step 7b)

## Summary

`ks-harness` drives the engine in one process from a line-based script. It loads synthetic signals or WAV/FLAC/MP3 files, decoded in full before playback, and at set times loads, plays, seeks, changes rates, and moves the mixer's controls. It plays either through a device (`OutputController` with `MiniaudioDeviceSource`) or into a WAV file through `render_offline()`, then prints a report on how the engine coped. It includes `src/apps/harness/scripts/soak.harness`, a one-hour soak script for step 8.

All 180 acceptance tests pass. The full `debug`, `asan`, and `rtsan` workflows passed in the final pass (355 tests; 357 under `rtsan`), and `tools/lint.sh` is clean. Pass 12 also ran the full `tsan`, `fuzz`, and `release` suites, and they passed. The fuzz target read 200,000 inputs cleanly. macOS is checked only in CI.

## What was built, task by task

1. **Encoder:** `encode()` is the inverse of `engine::decode()`. Global commands get target 0, reals are stored as bit patterns, and unused payload words are zeroed. Every command type survives the round trip.
2. **Script reader, part 1:** a private lexer handles quoted `file` paths, `\r\n` line endings, and `#` comments. Number parsing is strict (no `+`, exponent, `nan`, `inf`, or leading or trailing `.`; a syntax check, then `std::strtod`). The reader also handles track lines with every source and its limits, table-driven timed commands (each one checked as `decode(encode(c))`), the `end` rules, the 86,400 s, 100,000-event, and 64-track limits, the 1 MiB limit, and a stable sort into run order.
3. **Seek and cue point:** after sorting, the reader walks the events in run order to give each `TrackTime` its deck's current track. A seek or cue point on an empty deck is an error on its line. `command_for()` computes seconds × the track's rate.
4. **Repeat blocks:** the reader checks the count before expanding, expands line by line and repetition by repetition so ties keep line order, and reports a missing `end repeat` on the `repeat` line.
5. **Signals:** sine, exponential sweep (closed-form phase), SplitMix64 noise, and Hann-windowed clicks, all peaking at −12 dBFS, in stereo.
6. **Files and the track set:** decoding goes through the private codec library. Errors name the file, and a sample rate outside the engine's limits is an error. The track set publishes each track as complete.
7. **The report:** `print_report()` in `run.h`'s format; `flags_problem()`.
8. **The player:** a private `Player` shared by both modes. It owns the state block, the engine, and the command ring. It gives events when they're due, holds back every event behind a full ring and counts each as delayed, collects releases, and reads the status block through a seqlock.
9. **File mode:** renders segment by segment up to each event frame (`llround(t × rate)`), at most `block_frames` frames per call, with a fixed `start_time` so files repeat byte for byte. A failed write is an error naming the file.
10. **Device mode:** prints the backend, device, format, and latency before starting. It polls at most every 5 ms, prints state changes and a status line every 60 s, and counts freewheel entries as device losses. At the end, or when stopped, it waits up to 100 ms for the engine to read every command. It runs clean under `rtsan` and `tsan`.
11. **The command line:** `usage()` and `parse_options()`, with the mode-dependent checks done after every argument is read.
12. **`run()` and the soak script:** `--help`, `--list-devices`, script errors as `<script>:<line>: <message>` (exit 2), `--check`, the unlisted-device check, both modes, the report, and exit codes 0, 1, 2, and 3. The soak script has five synthetic tracks at 22,050 to 96,000 Hz on four decks, a 60 × 60 s block of loads, seeks, plays, pauses, an unload, and rate changes (reverse included), and a 720 × 5 s crossfader block, with comments on using music files instead.

## Deviations and choices

There were no deviations from the spec's requirements. The plan records each choice where a requirement left room:

- A `load` may name a track whose line comes later in the script (requirement 8 allows track lines anywhere), so names are resolved after every line is read.
- A quote may only start and end a whole token, and only a `file` path may be quoted.
- A seek's or cue point's seconds are capped at 86,400, so for a file track `decode()` accepts the frame at any rate. This confirms the plan's decision on file-track rates against `src/engine/command.cpp`.
- An empty repeat block is allowed and adds nothing.
- "Delayed" counts every event that was due but couldn't be given at that moment, the blocked one and every due one behind it, each once.
- When stopped early, the harness waits up to 100 ms for the engine to read every command, as it does at the end time.
- `--check` lists no devices, so it doesn't check `--device`. It opens no device and can run where no backend initializes.
- A script error on line 0 prints as `<script>:0:`.
- In the options: an option given twice keeps the last value; `-h` is accepted as `--help`, as in `ks-tone`; and a second script or an empty `--out` is a usage error.
- miniaudio refuses to open a WAV above 384 kHz, so the harness's own sample-rate error can only be reached for rates below the lower limit.

## Changed after the loop

Reviewing the loop's work turned up two gaps in the spec. The maintainer approved amending it, and the fixes were made interactively, with their tests:

- **Late blocks count as a problem only in device mode** (requirement 28). File mode renders as fast as it can, and it splits blocks at events, so a block can be a few frames long and take longer to render than it lasts. A debug build rendering four decks at the default 256-frame blocks reported a late block and exited with 3. File mode still prints late blocks and peak load. `Report::real_time` marks a device-mode report.
- **A synthetic track can't be longer than `kMaxTrackFrames`** (requirement 8). The spec allowed 3,600 seconds at any rate, but a deck refuses a track over 2^28 frames, without telling the harness, so that deck played nothing and the report didn't say so. It's now a script error on the `track` line. At 96 kHz the longest signal is about 2,796 seconds; at 44.1 kHz the 3,600-second limit still comes first.
- The soak script's comment says the crossfader jumps between 0.2 and 0.8 every 2.5 seconds, which the mixer smooths, rather than sweeping.

## Learnings worth adding to `CLAUDE.md`

- `render_offline()` writes non-interleaved per-channel buffers, and the WAV writer wants interleaved samples.
- The engine reads only 32 commands per block. Any client that sends a burst must keep rendering or polling until its acknowledgement catches up, or the report shows commands unread.
- An acceptance test can pass by accident while everything around it still fails (`ks-harness.unlisted-device` did until task 11). When one starts passing early, check that it passes for the right reason.

## To check by hand

- [ ] `[manual]` **Listening:** play the example soak script for a few minutes, first through the laptop and then through the Inpulse 500. It should sound as its comments describe, without clicks or dropouts.
- [ ] **CI on macOS arm64** (`debug`, `release`, `asan`, `tsan`): build loops build only on Linux.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
