# Feature: development harness

- **Status:** Approved by the maintainer on 2026-11-13; requirements 8 and 28 and the errors, delays, and device-mode criteria amended the same day, after the build loop's review and CI on macOS. Protected: build loops can't change it.
- **Roadmap:** Phase 1a (step 7b)
- **Built by:** loop. The spec, the API, the acceptance tests, the fuzz target, and the codec library are written interactively first.

## Goal

`ks-harness` drives the engine, in one process, from a script. It loads tracks (synthetic signals, or WAV, FLAC, and MP3 files), and at set times plays, seeks, changes rates, and moves the mixer's controls. It plays the script either through a device or into a WAV file, and reports how the engine coped. It's for listening and soak tests, including the Phase 1a soak and cable-pull criteria (step 8). It isn't shipped.

## Context

- **Decisions that apply:**
  - ADR 0003 §16: Phase 1a includes "a development harness for listening and soak tests".
  - ADR 0003 §4 and §10: clients send commands through a command ring, and tracks reach the audio thread through the give ring (`Engine::load()`). The harness is a client like any other, and uses only the engine's public interface.
  - ADR 0003 §9 and the reliability rules: the engine never reads files during playback, so every track is decoded or generated, in full, before playback starts.
  - ADR 0001: decoding uses miniaudio's own decoders. No new dependency.
- **The maintainer's design choices** (2026-11-12):
  - The script is line-based text, with absolute times in seconds and a repeat directive. The parser is fuzzed.
  - Decoding (WAV, FLAC, MP3) and WAV writing come from a second, private copy of miniaudio. It's compiled only into the harness, with `MA_API` set to `static` and with `MA_NO_DEVICE_IO`.
  - `ks::miniaudio`, which the engine links, keeps its codecs compiled out, so the engine carries no dead code.
  - A probe showed the two copies link into one program once the one global that ignores `MA_API` is renamed: `#define ma_atomic_global_lock ks_codecs_ma_atomic_global_lock`.
- **What exists:**
  - **`Engine`:**
    - `load()`, `unload()`, `attach()` for a client's `ipc::CommandRing`, and `collect()` for releases;
    - a state block (`ipc::EngineState`) that it publishes after every block;
    - `engine::decode()` validates a command slot; nothing encodes one yet.
  - **Rendering:** `RenderCore`, and `render_offline()` for rendering into memory as fast as possible.
  - **Devices:** `OutputController` with `MiniaudioDeviceSource` (miniaudio-devices spec). It plays through a chosen device, freewheels while the device is lost, and goes back to it when it returns.
  - **Layout to copy:** `ks-tone` puts everything except argument handling in a library, with a thin `main.cpp`.
  - **For tests:** `tests/acceptance/deck/wav.h`, a test-only WAV reader and writer.
- **Underruns:** miniaudio doesn't report them. An underrun the engine causes is a block that takes longer to render than it lasts, which the render core counts as a late block. The report gives those.

## Requirements

### The executable

1. **`ks-harness`** lives in `src/apps/harness/`: a library, `ks::harness`, with everything except argument handling, and a thin `main.cpp`. It isn't installed or packaged.
2. **Usage:** `ks-harness [options] <script>`.
   - `--out <file>`: render into a WAV file instead of playing through a device.
   - `--backend <name>`: a backend name as `parse_backend()` reads it; default `system`.
   - `--device <n>`: a device number from `--list-devices`; default: the default device.
   - `--list-devices`: list the backend's output devices, numbered from 0, with the default marked, and exit. No script needed.
   - `--rate <hz>`: the output sample rate, 8,000 to 384,000. File mode: default 48,000.
   - `--channels <n>`: output channels, 2 to 8. File mode: default 2.
   - `--period <frames>`: 1 to 4,096. Device mode: the device's period. File mode: the block size; default 256.
   - In device mode, `--rate`, `--channels`, and `--period` also accept 0, the default: the device's or the backend's choice.
   - `--check`: read the script and prepare its tracks, print a summary (tracks, events, end time), and exit without opening a device or a file.
   - `--help`: print the usage and exit.

   `--backend` or `--device` with `--out` is a usage error, and so is a device number that isn't listed.
3. **Exit codes:**
   - **0:** it ran to the end of the script, or was stopped early (requirement 26), and the report flags nothing; or `--list-devices`, `--check`, or `--help` succeeded.
   - **1:** a runtime error, such as a file that couldn't be decoded or written, or a backend that couldn't be initialized. The message names what failed.
   - **2:** a usage error, or an error in the script. Script errors print as `<script>:<line>: <message>`.
   - **3:** it ran, but the report flags a problem (requirement 28).

### The script

4. **Lines:**
   - A script is UTF-8 text of at most 1 MiB, read line by line. A line may end in `\r\n`.
   - `#` starts a comment that runs to the end of the line, except inside a quoted path. Blank lines and comment-only lines are ignored.
   - Tokens are separated by spaces or tabs. Keywords are lower case.
5. **Numbers:**
   - **Whole numbers** (counts, deck and channel numbers, sample rates): digits only.
   - **Amounts** (times, lengths in seconds, frequencies, BPM): digits with an optional fraction, such as `12` or `0.25`.
   - **Values** (rates, positions, transitions, decibels): an amount with an optional leading `-`. Decibels also accept `-inf`.
   - Nothing else is a number: no `+`, exponent, `nan` or `inf`, and no leading or trailing `.`.
6. **Reading a script does no other I/O.** It opens no track files: those are read when tracks are prepared (requirement 21).
7. **Decks and output channels are numbered from 1,** as on hardware: decks 1 to 4, and output channels 1 to 8. The harness converts them to the engine's 0-based indices.
8. **Tracks:** `track <name> <source>` names a track.
   - **The name** is a letter or `_`, then letters, digits, `_` or `-`. It's unique in the script.
   - **Where:** a script has at most 64 tracks. Track lines may appear anywhere outside a repeat block.
   - **Sources:**
     - `file "<path>"`: a WAV, FLAC, or MP3 file. The path is in double quotes and can't contain one. A relative path is relative to the script's directory.
     - `sine <hz> <seconds>`: a sine wave.
     - `sweep <from-hz> <to-hz> <seconds>`: a sine whose frequency moves exponentially from one to the other over the track.
     - `noise <seconds>`: white noise.
     - `clicks <bpm> <seconds>`: a click on every beat, starting at 0.
   - **Synthetic sources** may end with `rate <hz>`, their sample rate: 8,000 to 384,000, default 44,100. Their limits:
     - frequencies are above 0 and below half the sample rate;
     - the BPM is above 0 and at most 1,000;
     - the length is above 0 and at most 3,600 seconds;
     - the track's frames, `round(length × rate)`, are at most `kMaxTrackFrames` (2^28), the longest track a deck accepts. That limits only rates above about 74.5 kHz: at 96 kHz, a signal is at most about 2,796 seconds long.
9. **Timed lines:** `<time> <command> <arguments>`, where the time is in seconds from the start. Commands:

   | Line | What it does |
   |---|---|
   | `load <deck> <track>` | `Engine::load()` |
   | `unload <deck>` | `Engine::unload()` |
   | `play <deck>`, `pause <deck>` | `SetPlaying` |
   | `rate <deck> <rate>` | `SetRate`, for example `1.02`, or `-1` for reverse |
   | `seek <deck> <seconds>` | `Seek`, to that time in the deck's track |
   | `cuepoint <deck> <seconds>` | `SetCuePoint`, at that time in the deck's track |
   | `trim <deck> <dB>` | `SetTrim` |
   | `fader <deck> <position>` | `SetFader` |
   | `assign <deck> a\|b\|thru` | `SetCrossfaderAssign` |
   | `pfl <deck> on\|off` | `SetChannelCue` |
   | `crossfader <position>` | `SetCrossfader` |
   | `curve <transition>` | `SetCrossfaderCurve` |
   | `main <dB>`, `headphones <dB>` | `SetMainLevel`, `SetHeadphoneLevel` |
   | `cuemix <position>` | `SetCueMix` |
   | `busmap <main> <cue>` | `SetBusMap`: each bus's first output channel |
   | `end` | the script ends |

10. **Seek and cue point** convert seconds to track frames at the sample rate of the deck's track. That's the track loaded by the latest earlier `load` on that deck, in run order (requirement 13), with no `unload` since. A `seek` or `cuepoint` on a deck with no track is an error.
11. **Every command is checked with `engine::decode()`** when the script is read, after encoding (requirement 17). A command it rejects is an error on that line, so the script's rules for values are the engine's.
12. **Repeat blocks:**

    ```
    <time> repeat <count> every <seconds>
    <offset> <command> <arguments>
    ...
    end repeat
    ```

    - **Timing:** repetition k, from 0 to count − 1, runs each line inside at `time + k × period + offset`.
    - **Limits:** the count is 1 to 100,000, and the period is above 0. Each offset is at least 0 and below the period.
    - **Contents:** only timed commands. Track lines, `end`, and nested repeats aren't allowed inside.
13. **Run order:** events run in time order. Events at the same time run in the order of their lines in the script; for a repeat block, that's the order of the lines inside it.
    - **One exception:** within a block, the engine applies loads and unloads before it reads commands (commands spec requirement 8). So a load takes effect before a command given in the same block, whatever their order in the script.
14. **The end:** a script has exactly one `<time> end`, with a time above 0. Every event, with repeats expanded, comes before it.
15. **Limits:** times are at most 86,400 seconds (24 hours). A script expands to at most 100,000 events.
16. **Errors** name the line, and say in words what's wrong. Reading stops at the first error.

### Commands

17. **`encode(command, sequence)`** turns an `engine::Command` into an `ipc::CommandSlot`. For every command type, `decode()` of the slot gives back the same command.
18. **The harness is one client.**
    - It attaches one command ring before playback starts, and numbers its slots 1, 2, 3, and so on.
    - It gives loads and unloads through `Engine::load()` and `Engine::unload()`.
19. **Delays:** an event can't be given while its ring is full: the command ring for a command, or the give ring for a load or unload.
    - **The event waits,** and so does every event after it, so events stay in order.
    - **Retrying:** the harness tries again after each block in file mode, or each poll in device mode.
    - **Counting:** events that had to wait are counted as delayed.
20. **Releases:** the harness collects the engine's releases while it runs. Tracks stay alive until the engine has stopped.

### Tracks

21. **Preparing:** before playback starts, the harness decodes or generates every track in full, and publishes it as complete. Each track is interleaved stereo 32-bit float at the track's own sample rate. Track IDs are 1, 2, 3, and so on, in script order.
22. **Files** decode through the private codec library (requirement 30), at the file's own sample rate.
    - **Channels:** mono plays on both channels; miniaudio mixes down more than two.
    - **Errors name the file:** one that can't be read or decoded, one whose sample rate is outside 8,000 to 384,000, and one longer than `kMaxTrackFrames`.
23. **Signals** peak at −12 dBFS, the same on both channels:
    - **sine:** starts at phase 0.
    - **sweep:** frequency `from × (to / from)^(t / length)`, with continuous phase.
    - **noise:** uniform, from a fixed seed. It's the same on every run and platform, so it can't come from a standard-library distribution, whose output differs between libraries.
    - **clicks:** on each beat, 10 ms of a 1 kHz sine under a Hann window, and silence between.

### Playing

24. **File mode** (`--out`) renders through `render_offline()` into a 32-bit float WAV file, at `--rate`, with `--channels`, in blocks of `--period` frames.
    - **Length:** the file holds `round(end × rate)` frames.
    - **Timing:** an event at time t runs at frame `round(t × rate)`. Rendering stops at that frame, the event is given, and the next block starts there.
    - **Repeatable:** the same script and options give the same file, byte for byte.
    - **Block size:** if no event is delayed and no more than 32 commands (`kCommandsPerBlock`) share a frame, the file is the same at every block size.
25. **Device mode** plays through `OutputController` and a `MiniaudioDeviceSource` with the chosen backend, device, rate, channels, and period.
    - **Before starting,** it prints the device's backend, name, sample rate, channels, period, periods, and output latency.
    - **Timing:** time 0 is when it starts the controller. It gives each event once the clock reaches the event's time, polling at least every 5 ms.
    - **While playing,** it prints each change of output state with its time. Every 60 seconds it prints a status line: time played, late blocks, and peak render load.
    - **At the end time,** it waits up to 100 ms for the engine to read every command sent, then stops the controller, which fades out.
26. **Stopping early:** the library takes a stop flag, and `main.cpp` sets it on SIGINT (Ctrl-C). When it's set, device mode stops within 100 ms, as at the end time, and file mode stops rendering at the next block.
27. **Status:** the harness builds the engine with a state block. It reads the engine's status and its client's acknowledgements from that block, as an external client would.

### The report

28. **After playing,** the harness prints a report and returns it from the library:
    - the time played (frames rendered ÷ rate), and whether it was stopped early;
    - blocks rendered, late blocks, and peak render load;
    - non-finite samples and limited frames;
    - commands sent, read by the engine, and rejected, and events delayed;
    - device losses: how many times the output started freewheeling (device mode).

    The report flags a problem, which gives exit code 3, if any of these happened:
    - there were non-finite samples or rejected commands;
    - the engine read fewer commands than were sent;
    - in device mode, there were late blocks.

    File mode reports late blocks and render load, but doesn't flag late blocks. It renders as fast as it can, so a block's render time says nothing about playback, and it splits blocks at events, so a block can be a few frames long and take longer to render than it lasts.

### Real time

29. **Nothing the harness adds runs on the audio thread.** Tracks are ready before playback, and commands are encoded and pushed on the control thread. The engine and the render core run as they do in production, with the output safety stage last.

### The codec library

30. **`ks::miniaudio_codecs`**, in `third_party/miniaudio/`, compiles miniaudio a second time:
    - with `MA_API` defined as `static` and with `MA_NO_DEVICE_IO`;
    - with `ma_atomic_global_lock` renamed.

    It exposes a small C interface that doesn't include `miniaudio.h`. The interface decodes a file to interleaved stereo float at the file's own rate, and writes a 32-bit float WAV file a block at a time.
31. **Only `ks::harness` links it.** `ks::miniaudio` keeps `MA_NO_DECODING` and `MA_NO_ENCODING`.

### An example

32. **`src/apps/harness/scripts/soak.harness`** is a one-hour soak script for step 8. It uses synthetic tracks, so it runs anywhere:
    - four decks, with tracks at different sample rates;
    - repeated loads, seeks, and rate changes, reverse included;
    - crossfader moves.

    Its comments show how to use music files instead.

## Acceptance criteria

The tests are in `tests/acceptance/harness/`, with small audio fixtures in `tests/acceptance/harness/data/`. Device tests use miniaudio's null backend.

- [ ] `[auto]` **Reading tracks and events:** a script with every track source and every command reads into the expected tracks and events. That covers the times, decks and channels converted to 0-based, the commands, and seek and cue-point frames at the track's rate. Events are in run order, with ties in line order. (test: "Script: tracks and timed commands")
- [ ] `[auto]` **Repeat blocks:** a block expands to count × lines events at the right times, interleaved in run order with the events around it. (test: "Script: repeat blocks")
- [ ] `[auto]` **Errors:** each of these is an error on the right line:
  - an unknown command or keyword; a bad number, including `nan`, `inf`, `+1`, `1e3`, `.5`, and `5.`; a sign where none is allowed;
  - deck 0 or 5, output channel 0 or 9, and an unknown or duplicate track name;
  - `seek` or `cuepoint` on a deck with nothing loaded, or after an unload;
  - a command that `decode()` rejects, such as `busmap 1 1` and `rate 1 -inf`;
  - no `end`, two `end`s, and an event at or after the end;
  - in a repeat: a count of 0, an offset that isn't below the period, a nested repeat, a track line or `end` inside one, and a missing `end repeat`;
  - more than 100,000 events, more than 64 tracks, a time over 86,400 s, and a file over 1 MiB;
  - a signal longer than `kMaxTrackFrames` frames at its rate (a signal of exactly that many is read).

  (test: "Script: errors name the line")
- [ ] `[auto]` **Encoding:** every command type, with values at its edges, survives `decode(encode(command))`. (test: "Encoder: every command survives decode()")
- [ ] `[auto]` **Signals:**
  - each signal has its length and sample rate;
  - the sine, sweep and noise peak at −12 dBFS within 0.1 dB, and clicks don't go above it;
  - the sine's frequency, and the sweep's at its start, middle and end, are within 1% of what they should be;
  - noise is identical from run to run;
  - clicks are on the beats, and silent between.

  (test: "Signals: sine, sweep, noise, and clicks")
- [ ] `[auto]` **Files:**
  - WAV (16-bit and float, mono and stereo), FLAC, and MP3 fixtures decode at their own rates, with mono on both channels;
  - a missing file, a file that isn't audio, and a sample rate out of range are errors naming the file.

  (test: "Tracks: decoding files")
- [ ] `[auto]` **File mode, timing:** a script that plays a sine from 0.5 s renders silence before frame `round(0.5 × rate)`, and the sine from there, with the deck's fade-in. The file has `round(end × rate)` frames, `--channels` channels, and `--rate`. (test: "File output: events land on their frames")
- [ ] `[auto]` **File mode, determinism:** rendering a script twice gives identical files, byte for byte. A script with no delays and at most 32 commands at any one time gives the same file at block sizes 1, 128, and 256. (test: "File output: the same script renders the same file")
- [ ] `[auto]` **Delays:** 300 commands at one time all reach the engine, in order. The report counts the delayed ones, and shows the engine read as many commands as were sent. A late block in a file-mode report doesn't flag a problem. (test: "File output: delayed commands")
- [ ] `[auto]` **Device mode:** a 1-second script on the null device plays for 1 s, ±100 ms. Its report shows no rejections, and as many commands read as sent, and a late block in it would flag a problem. In a build without a sanitizer, it shows no late blocks: a sanitizer's overhead can make a block take longer to render than it lasts. The output names the device and shows its state changes. (test: "Device: playing a script on the null backend")
- [ ] `[auto]` **Stopping early:** setting the stop flag 200 ms into a 10-second script returns within 500 ms, with a report that says it was stopped early. (test: "Device: stopping early")
- [ ] `[auto]` **The command line:** (CTest cases in `tests/acceptance/harness/CMakeLists.txt`)
  - `--list-devices --backend null` lists `0: NULL Playback Device (default)`;
  - a bad option or option value exits 2 and prints the usage;
  - a script error exits 2 and prints `<script>:<line>:`;
  - `--check` on the example soak script exits 0;
  - `--out` renders a file and exits 0.
- [ ] `[auto]` **Fuzzing:** `ks_harness_script_fuzz` reads 200,000 inputs without a crash, a sanitizer report, or a timeout. For every script it accepts:
  - the events are in run order, before the end, and within the limits;
  - they're on valid decks and tracks;
  - every command decodes.

  (test: the fuzz target, under the `fuzz` preset)
- [ ] `[auto]` **Real time:** the device tests run clean under `rtsan`.
- [ ] `[manual]` **Listening:** the example soak script sounds as it describes, without clicks or dropouts, when played for a few minutes through the laptop and then through the Inpulse 500.
- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux and on macOS arm64 (`debug`, `release`, `asan`, `tsan`).

## Out of scope

- The one-hour soak and the cable pull themselves (step 8).
- Reporting underruns the operating system causes: miniaudio doesn't report underruns.
- Controlling the engine through MIDI, a socket, or another process (Phase 1b's `ks-play`).
- Decoding while playing, and tracks too big for memory.
- Following the system default device, and a fallback device.
- Loops, beat sync, and anything else the engine doesn't do yet.
- Variables, expressions, includes, and relative times in scripts.

## Open questions

None.
