# Feature: Render core and the first drivers

- **Status:** Approved by the maintainer on 2026-11-10. Protected: build loops can't change it.
- **Roadmap:** Phase 1a
- **Built by:** interactive

## Goal

Build the engine's one render function and the first two drivers that call it: offline, for tests, and miniaudio, for devices. Every later engine step plugs into this core, and every engine test renders through it.

## Context

- **ADR 0003 sections that apply:** §1 (one render function, several drivers, the render token), §2 (the audio thread), §3 (callback size, block-size invariance, sample rate, host time), §10 (scratch buffers allocated before the audio thread runs), and §15 (thread priority comes from the backend). The real-time rules in `CLAUDE.md` also apply.
- **What exists:** `ks::core` provides `KS_NONBLOCKING`. `ks::miniaudio` builds miniaudio. `ks-tone` plays through miniaudio directly; it stays a standalone tool and doesn't move onto the engine. `ks::ipc` isn't needed yet: commands arrive in a later step.
- **Step 2 is split in two.** This spec (2a) covers the render core, the offline driver, the `AudioBackend` interface, and the miniaudio backend. Step 2b adds the paced offline backend, the freewheel driver, simulated device loss, and reattaching.
- **What renders.** The core runs a `Graph`. Later steps make the decks and the mixer a graph. In this step, only tests implement one, so the miniaudio backend plays silence outside the tests.
- **The output safety stage** (step 3) goes inside `RenderCore`, after the graph, so no driver can bypass it.
- **The API exists before the code:** the acceptance tests compile against the public headers. Until the maintainer approves this spec and the tests, the function bodies are stubs, and every acceptance test fails except the compile-time checks.

## Requirements

### Library

1. A new library, `ks::engine`, in `src/engine/`, with public headers `<ks/engine/block.h>`, `<ks/engine/render_core.h>`, `<ks/engine/offline.h>`, `<ks/engine/audio_backend.h>`, `<ks/engine/miniaudio_backend.h>`, and `<ks/engine/interleave.h>`. It depends on `ks::core` and the standard library, and privately on `ks::miniaudio`. It doesn't depend on Qt.
2. **Reading the clock on the audio thread.** `ks::core` gains `<ks/core/clock.h>`, with `monotonic_now()`. It returns `std::chrono::steady_clock::now()` and is `noexcept KS_NONBLOCKING`.
   - Function effect analysis can't see that reading the clock doesn't block, and rejects the call. This function, and only this function, suppresses `-Wfunction-effects` around that one call, with a comment saying why.
   - RealtimeSanitizer still checks it at run time, and accepts the clock read.
   - The maintainer approves this suppression by approving this spec.

### Blocks

3. `kMaxBlockFrames` is 512 and `kMaxOutputChannels` is 8.
4. **`Block`** holds:
   - the output channels, one `std::span<float>` per channel (planar);
   - the frame count;
   - the sample rate;
   - the host time of its first frame, a `std::chrono::steady_clock::time_point`.

   A block is valid when it has 1 to `kMaxBlockFrames` frames, 1 to `kMaxOutputChannels` channels, a nonzero sample rate, and every channel holds at least as many samples as it has frames.
5. **`frames_to_duration(frames, sample_rate)`** returns floor(frames × 10⁹ / sample_rate) nanoseconds. It's exact, without overflow, for any frame count up to about 292 years of audio. A sample rate of 0 gives 0. It's `noexcept KS_NONBLOCKING`.

### Render core

6. **`Graph`** is what the core renders: `process(const Block&) noexcept KS_NONBLOCKING` writes the block's channels.
7. **`RenderCore`** holds a reference to a graph, and the render token. `try_render(const Block&)` returns one of:
   - **`Rejected`:** the block isn't valid. It doesn't touch the channels or call the graph.
   - **`Skipped`:** another driver holds the render token. It fills the block's channels with silence and doesn't call the graph.
   - **`Rendered`:** it takes the token, fills the block's channels with silence, calls the graph, adds the block's frames to its frame count, and releases the token.

   It takes the token with acquire ordering and releases it with release ordering, so each render sees the state the last one left, whichever thread that ran on.
8. **`frames_rendered()`** returns the total number of frames in rendered blocks. Any thread may call it.
9. `try_render()` and `frames_rendered()` are `noexcept KS_NONBLOCKING`.

### Offline driver

10. **`render_offline(core, config, output)`** renders into the caller's planar buffers, on the caller's thread. `config` holds the sample rate, a block size, and the host time of the first frame.
    - It renders every frame of `output`, in order, in blocks of the configured size. It splits any block larger than `kMaxBlockFrames` into pieces of at most `kMaxBlockFrames`.
    - The block that starts at frame k has host time start + `frames_to_duration(k, sample_rate)`.
    - Skipped blocks are silent (requirement 7).
    - It returns the number of frames in rendered blocks.
11. `render_offline()` renders nothing, leaves `output` untouched, and returns 0 if any of these is true:
    - `output` has no channels, or more than `kMaxOutputChannels`;
    - its channels differ in length;
    - the block size is 0;
    - the sample rate is 0.
12. `render_offline()` is `noexcept KS_NONBLOCKING`.

### Audio backends

13. **`AudioBackend`** is the interface the control thread will use for any device-like driver:
    - `start()` returns nothing or a `BackendError`;
    - `stop()` is `noexcept`;
    - `info()` returns a `StreamInfo`: backend name, device name, sample rate, channel count, and period size in frames.

    Once `stop()` returns, the backend calls `try_render()` no more until it's started again. Starting after a stop resumes rendering. Destroying a backend stops it.
14. **`open_miniaudio(core, config)`** opens the default playback device with miniaudio and returns it as an `AudioBackend`, not yet started.
    - `config` chooses miniaudio's null backend or the system's backends in miniaudio's default order. It may also ask for a channel count, a sample rate, and a period size. 0 means the device's or backend's own choice.
    - The stream is 32-bit float, with `noFixedSizedCallback` and miniaudio's low-latency profile.
    - It allocates everything the callback uses: scratch for `kMaxOutputChannels` × `kMaxBlockFrames` samples.
15. `open_miniaudio()` returns a `BackendError`, with a kind and a message, when:
    - no backend can be initialized;
    - the device can't be opened;
    - the channel count, whether requested or the device's own, exceeds `kMaxOutputChannels`.

    `start()` returns one when the device won't start.
16. **`interleave(planar, frames, interleaved)`** writes sample f of channel c to `interleaved[f × channels + c]`. If `interleaved` is shorter than frames × channels, or a channel is shorter than `frames`, it writes nothing and returns false. It's `noexcept KS_NONBLOCKING`.
17. **`render_interleaved(core, scratch, interleaved, channels, sample_rate, host_time)`** does a device driver's work for one callback, so it can be tested without a device:
    1. it splits the `interleaved.size() / channels` frames into blocks of at most `kMaxBlockFrames`; the block that starts at frame k has host time `host_time` + `frames_to_duration(k, sample_rate)`;
    2. it calls `try_render()` for each block, on the first `channels` scratch buffers;
    3. it interleaves each block into its place in `interleaved`;
    4. it returns the number of frames in rendered blocks.

    If its arguments are inconsistent, it fills `interleaved` with silence and returns 0. They're inconsistent when:
    - `channels` is 0 or more than `kMaxOutputChannels`;
    - `scratch` has fewer than `channels` buffers, or one of them holds fewer than `kMaxBlockFrames` samples;
    - `interleaved.size()` isn't a multiple of `channels`;
    - the sample rate is 0.

    It's `noexcept KS_NONBLOCKING`, and declared in `<ks/engine/interleave.h>`.
18. **The miniaudio callback** reads `monotonic_now()` once, then calls `render_interleaved()` on its scratch buffers and the device's buffer. It's `noexcept KS_NONBLOCKING`.

## Acceptance criteria

The tests are in `tests/acceptance/render-core/`. Tests that wait on a device fail, rather than hang, when nothing makes progress for 5 seconds.

Render core (`render_core_test.cpp`):

- [ ] `[auto]` A rendered block's channels are cleared before the graph runs: a graph that writes nothing leaves silence in channels that held other values. `frames_rendered()` grows by the block's frames. (test: "Render core: channels are cleared, then rendered")
- [ ] `[auto]` Each kind of invalid block is rejected: no frames, more than `kMaxBlockFrames`, no channels, more than `kMaxOutputChannels`, a channel shorter than the frame count, and a sample rate of 0. The channels are untouched, the graph isn't called, and `frames_rendered()` doesn't change. (test: "Render core: invalid blocks are rejected")
- [ ] `[auto]` A block that arrives while another is rendering is skipped and silenced: a graph that calls `try_render()` from inside `process()` gets `Skipped`, and that block's channels are silent. (test: "Render core: a block that arrives mid-render is skipped")
- [ ] `[auto]` Two threads each render 100,000 blocks through one core. The graph never runs on both at once, every block is either rendered or silent, and `frames_rendered()` equals the frames the two threads report rendering. (test: "Render core: only one driver renders at a time")
- [ ] `[auto]` `frames_to_duration()` is exact at 44.1, 48, 96, and 192 kHz, for 0 frames, 1 frame, one frame short of a second, one second, and 2⁴⁰ frames, and gives 0 at a sample rate of 0. (test: "Render core: frames_to_duration is exact")

Offline driver (`offline_test.cpp`):

- [ ] `[auto]` With a block size of 4096, the graph sees every frame once, in order, in blocks of at most `kMaxBlockFrames`, each with the configured sample rate and channel count, and the host time its first frame should have. (test: "Offline: every frame, in blocks of at most kMaxBlockFrames")
- [ ] `[auto]` A graph whose output depends only on the frame number renders bit-identical output with block sizes of 1, 37, 128, 512, and 4096. (test: "Offline: output doesn't depend on block size")
- [ ] `[auto]` Each kind of invalid configuration renders nothing, leaves the output untouched, and returns 0. (test: "Offline: invalid configurations render nothing")

Miniaudio backend (`miniaudio_backend_test.cpp`):

- [ ] `[auto]` On miniaudio's null backend, with 2 channels at 48 kHz:
  - `info()` reports what was asked for;
  - once started, the graph receives blocks of 1 to `kMaxBlockFrames` frames with that channel count and sample rate, until at least 0.1 s of audio has been rendered;
  - every block's host time lies between the moment `start()` was called and 1 s after `stop()` returned;
  - once `stop()` returns, the graph receives nothing for the next 100 ms;
  - starting again resumes rendering.

  (test: "Miniaudio: renders through the null backend")
- [ ] `[auto]` Asking for `kMaxOutputChannels` + 1 channels fails with the matching error kind. (test: "Miniaudio: refuses more channels than the engine renders")
- [ ] `[auto]` `interleave()` puts every channel's samples in place for 1, 2, and 8 channels, and writes nothing when either buffer is too short. (test: "Interleave: samples land in place")

Device render (`render_interleaved_test.cpp`):

- [ ] `[auto]` `render_interleaved()` renders 2000 frames of 3 channels in blocks of at most `kMaxBlockFrames`, each with host time `host_time` + `frames_to_duration()` of the frames before it, and every sample lands in its place in the interleaved buffer. Each kind of inconsistent argument fills the buffer with silence and returns 0. (test: "Device render: blocks, host times, and interleaving")

Real-time safety (`realtime_test.cpp`):

- [ ] `[auto]` `monotonic_now()`, `frames_to_duration()`, `try_render()`, `frames_rendered()`, `render_offline()`, `interleave()`, and `render_interleaved()` are called from a `KS_NONBLOCKING` function, so function effect analysis checks them at compile time. Under the `rtsan` preset, RealtimeSanitizer checks the whole render path at run time, including the miniaudio callback in the null-backend test. (test: "Render path is real-time safe")

Everywhere:

- [ ] `[auto]` All of the above pass in every CI configuration: every preset on Linux, and `debug`, `release`, `asan`, and `tsan` on macOS arm64.
- [ ] `[manual]` On Linux, the hidden test case "Listen: a tone through the default device" plays a 440 Hz tone at −20 dBFS for 3 seconds through the engine's miniaudio backend, with a 128-frame period. It sounds clean, without clicks or dropouts, through the laptop's output and through the Inpulse 500. macOS waits until there's a Mac build environment.

## Out of scope

- Device loss, the paced offline backend, the freewheel driver, block status flags, and reattaching (step 2b).
- The control thread, and miniaudio's notification callback (step 2b).
- Choosing a device other than the default, and listing devices (the development harness, step 7).
- The output safety stage (step 3), decks (step 4), the mixer (step 5), and commands and state out (step 6).
- Smoothing host times with a delay-locked loop, and output latency (the visual play position).
- Input devices.
- Thread priority: device threads get whatever their backend gives them (ADR 0003 §15).
- Moving `ks-tone` onto the engine.

## Open questions

None.
