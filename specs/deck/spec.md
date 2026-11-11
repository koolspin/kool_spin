# Feature: Deck and interpolator

- **Status:** Approved by the maintainer on 2026-11-11. Protected: build loops can't change it.
- **Roadmap:** Phase 1a
- **Built by:** interactive

## Goal

Play a track from memory the way a DJ deck does: play and pause, seek, a cue point, and variable rate including reverse, through one interpolating resampler that also converts the track's sample rate to the device's. Starting, stopping, and seeking never click. The deck never reads track memory that isn't decoded yet.

## Context

- **Decisions that apply:**
  - ADR 0003 §3: block-size invariance, and decks keep their position in track frames, so a device rate change changes only the step per output frame.
  - ADR 0003 §4: commands carry absolute values, so the deck's controls do too.
  - ADR 0003 §8 and §9: track memory is interleaved stereo 32-bit float at the file's rate. Playback can start before decoding finishes, and the engine never reads past `frames_ready`.
  - ADR 0003 §10: nothing on the audio thread allocates. The deck holds no memory of its own beyond fixed members.
  - ADR 0003 §11: the resume state holds each deck's track, position, rate, play state, and cue point, so the deck reports them.
  - The real-time rules in `CLAUDE.md`.
- **The maintainer's design choices** (2026-11-11):
  - a windowed-sinc interpolator;
  - gain ramps for play and pause, and a crossfade for seeks;
  - the deck stops at the ends of a track;
  - smoothed rate changes.
- **Why 64 taps:** a Python prototype of the kernel in requirement 6 measured, for a 44.1 kHz track at 48 kHz, residuals of −110 dB at 1 kHz and −91 dB at 18 kHz, with a flat response to 20 kHz. At rate 2, a 13 kHz tone that would land above the output's Nyquist frequency came out at −83 dB. Half the taps lost 0.4 dB at 20 kHz and let that 13 kHz tone through at −24 dB.
- **What exists:** the render core, the drivers, and the safety stage (Phase 1a steps 2 and 3). `Graph` is the interface the render core calls; a test wraps a deck in one. The mixer (step 5) will own the decks and sum their output.
- **How the deck is driven, for now:** by calling its methods on the audio thread between blocks. The command decoder (step 6) will call the same methods. Loading a track will go through the give ring then, and released tracks through the retire ring (ADR 0003 §10). Until then, the deck reports which tracks it still holds.

## Requirements

### Track memory

1. A new public header, `<ks/engine/track.h>`, holds `Track`: a view of decoded track memory (ADR 0003 §9).
   - **Construction:** from the samples, interleaved stereo 32-bit floats, and the track's sample rate. Its capacity is half the number of samples, in frames. The memory must outlive the `Track`, and only the frames published as ready are read.
   - **Decoding progress:** `publish(frames, complete)` is called by whatever decodes the track. It stores both values in one 64-bit word, with release ordering. `complete` means no more frames will come.
   - **Reading progress:** `ready()` reads that word once, with acquire ordering, and returns the frame count, clamped to the capacity, and the complete flag. A new `Track` has 0 frames ready and isn't complete.
2. **Limits**, in the same header:

   | Constant | Value | Meaning |
   |---|---|---|
   | `kMinTrackRate` | 8,000 | The lowest track sample rate a deck accepts. |
   | `kMaxTrackRate` | 384,000 | The highest. |
   | `kMaxTrackFrames` | 2²⁸ | The largest capacity a deck accepts: 2 GiB of samples (ADR 0003 §9). |

### The interpolator

3. A new public header, `<ks/engine/interpolator.h>`, holds `Interpolator`: the kernel table that decks share.
   - It allocates the table when it's constructed, so it must be constructed off the audio thread.
   - After that it never changes, and any number of decks can use one at once.
4. **What it computes.** The value of a track at a position x, in track frames, for each channel, is Σₖ hₛ(k − x) · sample[k], over the integers k with |k − x| < 32s.
   - **Samples outside the ready frames:** a frame k below 0, or at or beyond the track's ready frame count, reads as 0. The interpolator never reads track memory outside the ready frames.
   - **The stretch,** s, is |step| (requirement 9) clamped to [1, 4]. Playing faster than the output rate allows lowers the kernel's cutoff by s and widens it to match, so the output doesn't alias. Beyond 4, the kernel stops widening.
   - **The reach:** `kInterpolatorReach` = 128 frames. No read is farther than that from x.
5. **The kernel.** hₛ(t) = (1/s) · sinc(t/s) · w(t / 32s) for |t| < 32s, and 0 otherwise.
   - sinc(u) = sin(πu) / (πu), and sinc(0) = 1.
   - The window is Kaiser with β = 8: w(v) = I₀(8√(1 − v²)) / I₀(8).
   - **The cutoff** is at the lower of the track's and the output's Nyquist frequency. The transition band straddles it, so aliasing is confined to about the top 8% of the band, above 20 kHz at 44.1 kHz.
6. **The table.** h₁ is tabulated at 512 points per frame over [0, 32], and interpolated linearly between points.
   - hₛ(t) is evaluated as h₁(|t| / s) / s.
   - The table holds exactly 1 at t = 0, and exactly 0 at the other integers. At s = 1 and an integer position, the output is therefore exactly the track's sample.

### The deck

7. A new public header, `<ks/engine/deck.h>`, holds `Deck`, constructed with a reference to an `Interpolator`.
   - Construction and destruction allocate nothing.
   - The deck is used from one thread at a time, the audio thread. Every member function is `noexcept KS_NONBLOCKING`.
   - **Command timing:** controls called between renders take effect from the next rendered frame.
8. **Rendering.** `render(left, right, sample_rate)` writes the deck's output, one frame for each element of `left`, at the device's sample rate.
   - **Invalid calls:** if `left` and `right` differ in size, are empty, or hold more than `kMaxBlockFrames`, or the sample rate is 0, it writes nothing and changes nothing.
   - **With no track loaded,** it writes silence.
9. **Position and step.** The deck keeps its position in track frames, as a 64-bit fixed-point number with 32 fractional bits.
   - **The step,** each frame, is the current rate (requirement 10) × the track's sample rate ÷ the device's sample rate, rounded to the nearest 2⁻³² frame.
   - **Motion:** a moving head (requirements 12 and 13) adds the step to its position each frame, in integer arithmetic, so positions are exact and don't depend on block size.
10. **Rate.** `set_rate(rate)` sets the target rate. 1 is normal speed, and negative rates play in reverse.
    - **Validation:** NaN and infinity are ignored. Other values are clamped to ±`kMaxRate`, which is 4.
    - **Smoothing:** each frame, the current rate moves toward the target by (target − current) × (1 − e^(−1 / (`kRateSmoothing` × sample rate))), where `kRateSmoothing` is 5 ms. Once it's within 10⁻⁹ of the target, it equals the target.
    - **While silent,** meaning the main head's gain is 0 and there's no outgoing head (requirement 12), the current rate equals the target at once.
    - **Initial value:** a new deck's rate is 1. Loading a track doesn't change it.
11. **Gain ramps.** Each head has a gain p / n.
    - n is `kDeclickTime`, 5 ms, in frames at the device's sample rate, rounded to the nearest frame. p is an integer from 0 to n that moves one step toward its target after each frame.
    - **Targets:** the main head's target is n while the deck is playing and not waiting (requirement 17), and 0 otherwise. The outgoing head's target is 0.
    - **A rate change:** if the device's sample rate changes so that p would exceed n, p becomes n.
    - **No ramp:** if n is 0, a gain is 1 when its target is n, and 0 otherwise.
12. **Heads.** The deck reads the track through a main head, and at most one outgoing head. Each has a track, a position, and a gain.
    - **Output:** each frame is the main head's gain × the value at its position, plus the outgoing head's gain × the value at its position, for each channel.
    - **The outgoing head** moves every frame, ignoring the ends (requirement 17), and is dropped once its gain reaches 0.
    - **Reporting:** the deck's position, track, and waiting state are the main head's.
13. **Playing.** `set_playing(bool)` sets whether the deck plays. With no track loaded, it's ignored.
    - **Motion:** the main head moves while the deck is playing, or while its gain is above 0. A pause therefore fades out while the track plays on, for 5 ms.
14. **Seeking.** `seek(frame)` moves the main head to a track frame.
    - **Validation:** NaN and infinity are ignored. Other values are clamped to [0, capacity]. With no track loaded, it's ignored.
    - **Silent:** if the main head's gain is 0, the main head moves there at once.
    - **Audible, without an outgoing head:** the main head becomes the outgoing head, keeping its gain. A new main head starts at the frame with gain 0, and ramps up while the deck is playing. A seek while playing is therefore a 5 ms crossfade.
    - **Audible, during a crossfade:** the seek waits until the outgoing head is dropped, and then applies. A later seek replaces a waiting one.
15. **Loading.** `load(track)` loads a track.
    - **Validation:** it returns false, and changes nothing, if the track's sample rate is outside [`kMinTrackRate`, `kMaxTrackRate`] or its capacity exceeds `kMaxTrackFrames`. Otherwise it returns true.
    - **Effect:** the deck pauses, its cue point becomes 0, and the main head moves to frame 0 of the new track, as a seek would (requirement 14): at once if the main head is silent, as a fade out if it's audible, or after the current crossfade.
    - **Unloading:** `unload()` does the same with no track.
    - **Waiting changes:** a later load or unload replaces a waiting load, and cancels a waiting seek. A later seek applies after a waiting load.
    - **Load and play:** a load followed by `set_playing(true)` crossfades from the old track to the new one.
16. **Cue point.** `set_cue(frame)` stores a cue point, validated like a seek, and `cue()` returns it. The deck doesn't act on it; clients implement cue buttons.
17. **The ends of a track.** These rules apply to the main head. "Ready" is the track's `ready()` (requirement 18).
    - **The start:** if the next position would be below 0, the head stops at 0, and the deck pauses.
    - **The end of a complete track:** if the next position would be past the ready frame count, the head stops there, and the deck pauses. A position past the end when a block starts, after a seek for example, moves to the end.
    - **An incomplete track:** the head never moves past the waiting limit, which is the ready frame count − `kInterpolatorReach`. It stops at the limit, or doesn't move if it's already past it. While it can't move as it otherwise would, the deck is waiting: `waiting()` is true and the deck stays playing. It moves on, and its gain ramps back up, once the limit allows.
    - **Stopping is silent:** while held at an end or waiting, the head stays still and its gain ramps to 0, so the output fades out on a constant value.
18. **Reading decoding progress.** The deck calls `ready()` once per block for each track its heads hold, and uses those values for the whole block.
19. **State.** These return the deck's state as of the last rendered frame, with the effect of any controls called since:
    - `track()`: the main head's track, or null;
    - `holds(track)`: whether either head, or a waiting load, refers to the track. A track must outlive the time it's held;
    - `playing()`, `waiting()`, and `rate()` (the target);
    - `position()`: the main head's position, in frames, as a `double`;
    - `cue()`.
20. **Block-size invariance.** Everything above works one frame at a time and carries its state across blocks. Given the same controls at the same frames and the same decoding progress, the output and the state don't depend on how the frames were divided into blocks (ADR 0003 §3).

## Acceptance criteria

The tests are in `tests/acceptance/deck/`. Unless a criterion says otherwise, a test renders a deck directly at 48 kHz, in blocks of at most 64 frames, split so that each control takes effect at its exact frame. At 48 kHz, a 5 ms ramp is 240 frames.

**Measuring a tone:** "Residual" means a least-squares fit of a sine at the expected output frequency (amplitude, phase, and offset) over 4,800 frames that start after any ramp. The residual is the rest of the signal's power, relative to the fitted sine's.

The interpolator (`interpolation_test.cpp`):

- [ ] `[auto]` **Unity:** a 48 kHz track played at rate 1 matches the track's samples exactly, from frame 240 on. Its position advances by exactly 1 per frame. (test: "Interpolation: unity rate is bit-exact")
- [ ] `[auto]` **Rate conversion:** sines at 0.5 amplitude, played at rate 1:
  - a 44.1 kHz track's sines at 1, 10, and 18 kHz come out at the same frequency, with their level within ±0.01 dB and a residual at most −80 dB;
  - the same for a 48 kHz track's sines at 1 and 10 kHz, played at 44.1 kHz.

  (test: "Interpolation: converting the track's rate")
- [ ] `[auto]` **Speeding up:** a 44.1 kHz track at rate 2:
  - a 5 kHz sine comes out at 10 kHz, within ±0.01 dB, with a residual at most −80 dB;
  - a 15 kHz sine, which would land at 30 kHz, above the output's Nyquist frequency, comes out at least 80 dB down.

  (test: "Interpolation: playing faster doesn't alias")
- [ ] `[auto]` **Reverse:** a 44.1 kHz track's 1 kHz sine played at rate −1 comes out as a 1 kHz sine with a residual at most −80 dB, and the position decreases. (test: "Interpolation: reverse")
- [ ] `[auto]` **Reading only decoded frames:** every sample in the track's memory beyond the ready frames is NaN, and memory beyond the capacity is NaN too. Every output sample stays finite while the deck:
  - plays forward into the waiting limit, at rates up to 4;
  - seeks past the ready frames;
  - plays in reverse into frame 0;
  - plays a complete track into its end.

  (test: "Interpolation: reads only decoded frames")

The deck (`deck_test.cpp`):

- [ ] `[auto]` **Playing and pausing**, on a 48 kHz track at rate 1:
  - after `set_playing(true)`, output frame k is the track's sample × k / 240, within 10⁻⁶, and the exact sample from frame 240 on;
  - after `set_playing(false)`, frame k is the sample × (240 − k) / 240, then silence. The position stops 240 frames after the pause.

  (test: "Deck: play and pause ramp the gain")
- [ ] `[auto]` **Seeking**, on a 48 kHz track at rate 1:
  - **While playing:** a seek to frame F crossfades, frame k being (1 − k/240) × the old head's sample + (k/240) × the new head's, within 10⁻⁶. `position()` is F + k.
  - **During a crossfade:** a second seek waits until frame 240 of the first, and then crossfades in the same way.
  - **While paused:** a seek moves the position at once.

  (test: "Deck: seeking crossfades")
- [ ] `[auto]` **Rate changes**, on a 48 kHz track at 48 kHz:
  - after `set_rate(2)` while playing, the position 0.1 s later is within 10⁻³ frames of the smoothing in requirement 10, computed in `double`. After 0.25 s, it advances by exactly 2 per frame;
  - while silent, `set_rate(2)` followed by `set_playing(true)` advances by exactly 2 from the first frame;
  - `set_rate(10)` makes `rate()` 4, and `set_rate(NaN)` changes nothing;
  - loading a track keeps the rate.

  (test: "Deck: rate changes are smoothed")
- [ ] `[auto]` **The ends**, on a complete 48 kHz track of 1,000 frames:
  - playing from frame 900 at rate 1, the position is exactly 1,000 from frame 100 on, `playing()` becomes false, and the output is silent;
  - playing in reverse from frame 500, the position is exactly 0 from frame 500 on, `playing()` becomes false, and the output fades on the held first sample: frame 500 + k is sample[0] × (240 − k) / 240, within 10⁻⁶;
  - playing at the end stops at once, while reverse from the end plays;
  - a seek to frame 5,000 moves the position to 1,000.

  (test: "Deck: stops at the ends")
- [ ] `[auto]` **Waiting**, on an incomplete 48 kHz track with 10,000 of its 48,000 frames ready:
  - playing from frame 9,000, the position holds at exactly 9,872, `waiting()` is true, and `playing()` stays true. The output fades on the held sample: sample[9,872] × (240 − k) / 240, within 10⁻⁶;
  - after 20,000 frames are published, the position moves on from 9,872, ramping up, and `waiting()` is false;
  - a seek to frame 30,000 waits there until all 48,000 frames are published as complete, and then plays from 30,000.

  (test: "Deck: waits for decoding")
- [ ] `[auto]` **Loading:**
  - **While playing:** the old track fades out over 240 frames. The new track is paused at frame 0 with cue point 0. `holds()` is true for the old track until the fade ends, and false after.
  - **Load and play** in the same block crossfade from the old track to the new.
  - **Refused:** tracks at 7,999 Hz and 384,001 Hz, and one with a capacity of 2²⁸ + 1 frames, are refused. The last is backed by address space reserved with `mmap`, never touched.
  - **Unloading while playing** fades out.
  - **With no track loaded,** the output is silent, and play, seek, and cue are ignored.

  (test: "Deck: loading and unloading")
- [ ] `[auto]` **Block-size invariance:** the performance script (below) renders bit-identical output and identical states with blocks of 1, 37, 128, and 512 frames. (test: "Deck: output doesn't depend on block size")
- [ ] `[auto]` **Golden file:** the performance script, rendered through a render core with the transparent safety configuration by the offline driver, one call between each pair of controls, matches `golden/performance.wav` within 10⁻⁵ per sample. (test: "Deck: the performance matches its golden file")
  - The file is 32-bit float stereo WAV at 48 kHz, so it can be listened to.
  - Running the test with `KS_UPDATE_GOLDEN=1` writes the file instead of checking it, and skips the test. A changed golden file is a protected change.

**The performance script** (`test_support.h`), on a synthetic 3.5 s, 44.1 kHz track of steady tones with a decaying click every 0.25 s, rendered for 3 s at 48 kHz:

| Time | Control |
|---|---|
| 0.00 s | Load the track, seek to 0.5 s of track time, play. |
| 0.50 s | Rate 1.08. |
| 0.90 s | Seek to 2.0 s. |
| 0.902 s | Seek to 1.0 s, while the first crossfade is running. |
| 1.30 s | Rate −1. |
| 1.80 s | Pause. |
| 2.00 s | Rate 1, play. |
| 2.20 s | Rate 4. |
| 2.40 s | Rate 1, seek to 3.4 s, which reaches the end and stops. |

Real-time safety (`realtime_test.cpp`):

- [ ] `[auto]` Every `Deck` member function, and `Track::ready()`, is called from a `KS_NONBLOCKING` function, so function effect analysis checks them, and the interpolator, at compile time. Under the `rtsan` preset, RealtimeSanitizer checks them at run time in every test that renders. (test: "Deck is real-time safe")

Everywhere:

- [ ] `[auto]` All of the above pass in every CI configuration: every preset on Linux, and `debug`, `release`, `asan`, and `tsan` on macOS arm64.
- [x] `[manual]` **Listening.** The hidden test "Listen: a deck through the default device" plays the file named by `KS_LISTEN_TRACK` (a WAV file: 16- or 24-bit PCM or 32-bit float, mono or stereo, at any rate), or the synthetic track if it's unset. It plays through the default device, with the default safety stage, and runs this script:
  - 10 s at rate 1;
  - 5 s each at rates 1.08, 0.92, and 1.5;
  - 3 s in reverse;
  - pauses and plays every 0.5 s for 3 s;
  - eight seeks 0.5 s apart, alternating between two points;
  - 3 s at rate 2.

  With a 44.1 kHz track, through the laptop's output and the Inpulse 500:
  - rate 1 sounds like the original;
  - rate changes are smooth, without zipper noise;
  - reverse is clean;
  - pauses, plays, and seeks don't click;
  - rate 2 has no aliasing whistles.

## Out of scope

- The command decoder and command rings, and the give and retire rings for tracks (step 6). Tests call the deck's methods directly.
- The mixer: gains, the crossfader, buses (step 5).
- State out: the deck's seqlock block, the visual play position, and the resume state (step 6).
- Keylock, loops, hot cues, beat grids, and sync.
- Cue-button behavior, such as CDJ-style cue: clients build it from seek, play, and the cue point.
- Turntable-style spin-up and brake.
- Decoding, track segments, and the decode worker (Phase 1b). Tests build tracks in private memory.

## Open questions

None.
