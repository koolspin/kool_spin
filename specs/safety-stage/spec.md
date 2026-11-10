# Feature: Output safety stage

- **Status:** Approved by the maintainer on 2026-11-10. Protected: build loops can't change it.
- **Roadmap:** Phase 1a
- **Built by:** interactive

## Goal

Make sure a bug never becomes a full-scale blast or a click through a PA (ADR 0002 §5). The last stage before the device silences NaN and Inf, caps the level, flushes denormals, and fades the output in and out. It runs inside the render core, after the graph, so no driver can bypass it.

## Context

- **Decisions that apply:**
  - ADR 0002 §5: what the stage does, and that it's always on.
  - ADR 0003 §1: blocks marked `resumed` fade in.
  - ADR 0003 §3: block-size invariance. The stage works one frame at a time and carries its state across blocks.
  - ADR 0003 §8: samples can be NaN or Inf.
  - The real-time rules in `CLAUDE.md`.
- **What exists:** `RenderCore::try_render()` silences each block, runs the graph, and marks `Block::resumed` (the render-core and device-loss specs). The output controller starts and stops the drivers.
- **Earlier tests must change.** Eight acceptance tests from the render-core and device-loss steps check rendered samples exactly: five compare the graph's signal sample for sample, and three expect a constant 1.0. With the stage's default settings, the first 10 ms fade in, and the limiter reduces anything above −1 dBFS. Requirement 18 says how those tests change. Approving this spec approves those edits.
- **What the stage can't do:** tell a loud mix from a bug producing full-scale noise. A limiter caps level; it doesn't judge content.
- **The API exists before the code:** the acceptance tests compile against the public headers. Until the maintainer approves this spec and the tests, the stage is a stub that changes nothing, and the new acceptance tests fail.

## Requirements

### Configuration

1. A new public header, `<ks/engine/safety.h>`, holds `SafetyConfig`:

   | Setting | Default | Meaning |
   |---|---|---|
   | `fade` | 10 ms | How long a fade in or out takes. 0 means changes are immediate. |
   | `ceiling_dbfs` | −1 dBFS | The most the output may reach. Above 0 is treated as 0. |
   | `release` | 100 ms | The limiter's release time constant. |

2. `RenderCore(graph, config)` takes a `SafetyConfig` as a second argument, defaulting to the defaults above. Production code uses the defaults.

### Processing

3. **Where it runs.** For every rendered block, after the graph and before `try_render()` returns, the core runs the stage over the block's frames, in order, one frame at a time. A frame is one sample from each channel. Skipped and rejected blocks don't pass through it, and its state carries from one rendered block to the next.
4. **Order within a frame:**
   1. non-finite samples become 0 (requirement 5);
   2. every sample is multiplied by the fade gain (requirement 6);
   3. every sample is multiplied by the limiter gain (requirement 7);
   4. every sample is clamped to ±ceiling;
   5. the fade gain takes one step toward its target.
5. **NaN and Inf.**
   - Each NaN or infinite sample becomes 0 and is counted.
   - A frame with any non-finite sample sets the fade position to 0 (requirement 6) before its gain is applied. Its output is therefore silent on every channel, and the output fades back in afterwards.
6. **Fades.**
   - The fade gain is p / n, where p is an integer fade position from 0 to n, and n is `fade` in frames at the block's sample rate, rounded to the nearest frame.
   - **The target** is 0 while muted (requirement 9), and n otherwise. After each frame, p moves one step toward it.
   - **Resets.** p becomes 0 at the start of a block marked `resumed`, and on a frame with a non-finite sample.
   - **Gain before the first block:** p starts at 0.
   - **No fade:** if n is 0, the gain is 1 when not muted and 0 when muted.
   - **A sample rate change:** if the rate changes so that p would exceed n, p becomes n.
7. **The limiter.**
   - **Attack:** the frame's peak is the largest absolute value across channels, after the fade gain. If the peak exceeds the ceiling, the target gain is ceiling / peak, and otherwise 1. If the target is below the current limiter gain, the gain drops to it at once.
   - **Release:** otherwise the gain moves toward the target by (target − gain) × (1 − e^(−1 / (release × sample rate))). Once it's within 10⁻⁴ of 1, it becomes 1.
   - **Counting:** frames in which the limiter gain is below 1 are counted.
8. **Denormals.**
   - While a block is rendered (the graph and the stage), the core sets the floating-point mode to flush denormals to zero: FTZ and DAZ on x86-64, and FZ on arm64.
   - It restores the previous mode before `try_render()` returns, so the calling thread's mode is otherwise unchanged.

### Control and counters

9. **Muting.**
   - `RenderCore::set_muted(bool)` sets the fade target: 0 while muted, and the full gain otherwise. Any thread may call it, and the change takes effect at the start of the next rendered block.
   - `silenced()` is true when muted and the fade gain has reached 0 by the end of the last rendered block.
10. **Counters.** `non_finite_samples()` and `limited_frames()` return the counts from requirements 5 and 7, as totals since the core was created. Any thread may call them. (The engine status block will publish them later.)
11. `set_muted()`, `silenced()`, `non_finite_samples()`, and `limited_frames()` are `noexcept KS_NONBLOCKING`, and so is everything the stage runs.
12. **Guarantee.** Every sample `try_render()` leaves in a rendered block is finite and within ±ceiling, whatever the graph wrote.

### The output controller

13. `OutputController::start()` unmutes the core before starting anything.
14. **`OutputController::stop()`:**
    1. mutes the core;
    2. while a driver is running, polls `silenced()` every millisecond until it's true, giving up after the fade time plus 100 ms;
    3. then stops and closes everything, as before.

    Stopping therefore fades out, unless nothing is rendering.

### Block-size invariance

15. Given the same graph output, the same configuration, and the same `resumed` marks, the stage's output and counters don't depend on how the frames were divided into blocks. A mute or unmute takes effect at a block boundary, like any live command (ADR 0003 §3).

### Earlier tests

16. Nothing in the render-core or device-loss specs changes, except as requirements 17 and 18 say.
17. The device-loss acceptance tests that call `OutputController::stop()` still pass with stop() fading out (requirement 14).
18. **Eight earlier acceptance tests change.** They construct their core with a transparent configuration, `SafetyConfig{.fade = 0, .ceiling_dbfs = 0}`, under which in-range samples pass through unchanged. The stage still runs. The tests:
    - render-core: "Render core: only one driver renders at a time", "Offline: every frame, in blocks of at most kMaxBlockFrames", "Offline: output doesn't depend on block size", "Offline: invalid configurations render nothing", and "Device render: blocks, host times, and interleaving";
    - device-loss: "Active driver: only the active driver renders", "Active driver: handovers under load", and "Paced: records what it plays".

    No other change is made to them.

## Acceptance criteria

The tests are in `tests/acceptance/safety-stage/`. Unless a criterion says otherwise, the core uses the default configuration and renders 2 channels at 48 kHz through `render_offline()`, in blocks of 64 frames. A 10 ms fade is then 480 frames. "Within 10⁻⁶" compares floats with that absolute tolerance.

The stage (`safety_stage_test.cpp`):

- [ ] `[auto]` With a graph writing 0.5:
  - the output fades in from the first frame, frame k being 0.5 × k / 480 within 10⁻⁶, and is exactly 0.5 from frame 480;
  - after the active driver changes, the new driver's output fades in again the same way.

  (test: "Safety: fades in after a start and after a handover")
- [ ] `[auto]` A graph writing 0.5, with a NaN at frame 1,000 on one channel, +Inf at frame 1,500 on the other, and −Inf at frame 2,000 on both:
  - **At the bad frames:** every channel is silent at each of those frames.
  - **After each:** the output fades back in, frame j after it being 0.5 × j / 480 within 10⁻⁶.
  - **Overall:** every sample is finite, and `non_finite_samples()` is 4.

  (test: "Safety: NaN and Inf become silence, then fade back in")
- [ ] `[auto]` A graph writing 0.5 for 0.2 s, then 2.0 for 0.1 s, then 0.5:
  - **During the burst:** every sample is the ceiling (−1 dBFS) within 10⁻⁶.
  - **Throughout:** no sample exceeds the ceiling.
  - **0.1 s after the burst:** the output is 0.5 × g, with g between 0.78 and 0.81 (one release time constant).
  - **1 s after the burst:** it's exactly 0.5.
  - **Counting:** `limited_frames()` is more than the burst's frames, and fewer than the total.
  - **Huge values:** a graph writing ±10³⁰ produces ±ceiling within 10⁻⁶, from the second frame on. The first frame of the fade-in has gain 0.
  - **Another ceiling:** a −6 dBFS ceiling is held the same way.

  (test: "Safety: the limiter holds the ceiling and recovers")
- [ ] `[auto]` **Denormals:**
  - inside the graph, `FLT_MIN × 0.5` comes out as 0;
  - a denormal the graph writes reaches the output as 0;
  - after `try_render()` returns, the same multiplication on the test's thread gives a denormal again.

  (test: "Safety: denormals are flushed while rendering")
- [ ] `[auto]` With a graph writing 0.5:
  - **Muting:** after `set_muted(true)`, the next block starts a fade to 0 over 480 frames: frame k is 0.5 × (480 − k) / 480 within 10⁻⁶. After that it's silent, and `silenced()` is true.
  - **Unmuting:** after `set_muted(false)`, the output fades back in over 480 frames, and `silenced()` is false.
  - **Muted from the start:** a core muted before its first block renders only silence, and `silenced()` is true.

  (test: "Safety: muting fades out, and unmuting fades back in")
- [ ] `[auto]` A graph writing 1.5 × a sine (so the limiter works), with NaNs at fixed frames, renders bit-identical output with block sizes of 1, 37, 128, 512, and 4096, and the same counters. (test: "Safety: output doesn't depend on block size")
- [ ] `[auto]` **The transparent configuration** (`fade` 0, ceiling 0 dBFS):
  - samples within ±1 pass through bit-exactly;
  - 1.5 becomes 1;
  - NaN still becomes 0;
  - denormals are still flushed.

  (test: "Safety: the transparent configuration still runs")
- [ ] `[auto]` **Stopping the output fades out first.** An output controller on a simulated device plays for 100 ms, then is stopped:
  - `silenced()` is true when `stop()` returns, and the core rendered at least 480 frames during it;
  - after `start()` again, the controller is `Playing`, the core isn't muted, and frames render.

  (test: "Safety: stopping the output fades out first")

Real-time safety (`realtime_test.cpp`):

- [ ] `[auto]` `set_muted()`, `silenced()`, `non_finite_samples()`, `limited_frames()`, and a rendering `try_render()` are called from a `KS_NONBLOCKING` function, so function effect analysis checks them, and the stage, at compile time. Under the `rtsan` preset, RealtimeSanitizer checks the stage at run time in every test that renders. (test: "Safety stage is real-time safe")

Earlier tests:

- [ ] `[auto]` The eight tests named in requirement 18 pass with the transparent configuration, and every other render-core and device-loss acceptance test passes unchanged. (tests: as named)

Everywhere:

- [ ] `[auto]` All of the above pass in every CI configuration: every preset on Linux, and `debug`, `release`, `asan`, and `tsan` on macOS arm64.
- [ ] `[manual]` The listening test from the render-core spec ("Listen: a tone through the default device") now runs through the stage with its defaults. It still sounds clean through the laptop's output and the Inpulse 500, with no click at the start.

## Out of scope

- Detecting runaway output that is loud but finite, such as a bug producing full-scale noise.
- A lookahead limiter, or any limiter that adds latency.
- Publishing the counters through the engine status block (a later step).
- Resetting recursive filters whose state stops being finite: there are none until EQ arrives.
- Fading out on device loss: the device is gone, so there's nothing to fade.

## Open questions

None.
