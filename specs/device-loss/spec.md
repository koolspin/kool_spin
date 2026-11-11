# Feature: Device loss

- **Status:** Approved by the maintainer on 2026-11-10. Protected: build loops can't change it.
- **Roadmap:** Phase 1a
- **Built by:** interactive

## Goal

Keep the music's clock running when the audio device disappears, and pick up at the right place when it comes back, the way a turntable keeps spinning when a cable is pulled (ADR 0002 §4). This step builds the machinery and proves it with simulated devices; the miniaudio side comes in step 2c.

## Context

- **ADR 0003 sections that apply:** §1 (drivers, the active driver, the render token), §2 (the control thread), §3 (host time), and §15 (device loss, real-time priority). This branch amends §1 and §15: only the active driver renders, the freewheel driver starts where the device stopped, and handing back to the device makes it active. ADR 0002 §4 says device loss is normal operation.
- **What exists:** the render-core spec's `ks::engine`: `RenderCore` and its render token, `Graph`, `Block`, `render_offline()`, `render_interleaved()`, `AudioBackend`, and the miniaudio backend.
- **Step 2 is split three ways.** 2a built the render core. This spec (2b) adds the active driver, the paced backend, the freewheel driver, simulated devices, and the output controller that handles loss and return. 2c, alongside the development harness in step 7, connects miniaudio: its notifications, recognising the same device when it returns, and reopening it. The cable-pull test (step 8) checks the result on hardware.
- **Fading in** belongs to the output safety stage (step 3). This step marks the blocks where it should happen.
- **The control thread** arrives with the engine process in Phase 1b. `OutputController::poll()` is the part of it that this step needs.
- **The API exists before the code:** the acceptance tests compile against the public headers. Until the maintainer approves this spec and the tests, the new function bodies are stubs, and every new acceptance test fails except the compile-time checks.

## Requirements

### Library

1. Everything is in `ks::engine`. New public headers: `<ks/engine/paced_backend.h>` and `<ks/engine/output_controller.h>`. Everything the render-core acceptance tests use keeps working unchanged.

### The active driver

2. **`DriverId`** identifies a driver to the render core. `kDefaultDriver` is active until another driver is chosen.
3. **`RenderCore::activate(driver)`** makes `driver` the active driver, and **`active_driver()`** returns it. Any thread may call them.
4. **`try_render(block, driver)`** takes the driver as a second argument, which defaults to `kDefaultDriver`. If `driver` isn't the active driver, it returns `Skipped`, with the block's channels silenced, as when the token is taken. It checks while holding the render token, so blocks from two drivers never overlap.
5. **`Block::resumed`** is set by the core on the block it passes to the graph: on the first block it renders, and on the first block from a driver other than the one that rendered last. Skipped blocks don't count. Drivers leave it false, and the core ignores what they set. The output safety stage (step 3) fades in on these blocks.
6. **`rendered_until()`** returns the host time at which the last rendered block ended: its host time plus `frames_to_duration()` of its frames. Before anything has rendered, it returns `time_point{}`. Any thread may call it.
7. `activate()`, `active_driver()`, and `rendered_until()` are `noexcept KS_NONBLOCKING`.

### The existing drivers

8. `OfflineConfig` gains `driver`, and `render_offline()` renders as that driver. `render_interleaved()` gains a last parameter, `driver`. Both default to `kDefaultDriver`.
9. **`AudioBackend`** gains:
   - `lost()`: true once the backend knows its device has gone;
   - `callbacks()`: how many times it has asked the core to render (device callbacks, or paced periods), whether or not anything rendered.

   Both are `noexcept`, and any thread may call them.
10. `MiniaudioConfig` gains `driver`, and the miniaudio backend renders as that driver and counts its callbacks. Its `lost()` stays false until step 2c handles miniaudio's notifications.

### The paced backend

11. **`open_paced(core, config)`** returns a `PacedBackend`, which is an `AudioBackend`, not yet started. `PacedConfig` holds:
    - the sample rate, channel count, and period (frames per period);
    - the driver it renders as;
    - how many frames to record (0: none);
    - whether to ask for real-time priority;
    - an optional start time;
    - an optional `SimulatedDevice` it plays on.

    It allocates everything its thread uses: an interleaved period buffer, scratch for `kMaxOutputChannels` × `kMaxBlockFrames` samples, and the recording. It returns a `BackendError` if the channel count is 0 (`OpenFailed`) or more than `kMaxOutputChannels` (`TooManyChannels`), or if the sample rate or period is 0 (`OpenFailed`).
12. **Pacing.** Once started, its thread renders one period at a time through `render_interleaved()`, as its driver.
    - Period n has host time start + `frames_to_duration(n × period, sample_rate)`. The start is the configured start time for the first start, and otherwise the time `start()` is called.
    - It renders each period at its host time, or as soon as it can after it. When it's behind, because its start time is in the past or its thread woke late, it renders the missed periods back to back until it has caught up.
    - Each period counts as a callback.
13. **Rendering a period** is a `noexcept KS_NONBLOCKING` function. Waiting for the next period is outside it.
14. **Recording.** If asked to record, it appends each period's interleaved output to the recording, silent periods included, until the recording holds the frames asked for. `recording()` returns what was recorded. It may be read once `stop()` has returned.
15. **Real-time priority.**
    - If asked, its thread asks for real-time priority when it starts:
      - on Linux, `SCHED_FIFO` at priority 70;
      - on macOS, a time-constraint policy whose period and constraint are one period, with half a period of computation.
    - `priority()` reports `NotRequested`, `Granted`, or `Refused`, once the thread has started.
    - Being refused isn't an error: the thread runs at its normal priority.
16. `info()` reports backend `"Paced"`, and the configured sample rate, channel count, and period. `stop()` returns once its thread has stopped; it doesn't render again until `start()`. Destroying it stops it.

### Simulated devices

17. **`SimulatedDevice`** stands in for hardware in tests. It's present when created.
    - `unplug(Loss::Silent)`: every paced backend playing on it stops rendering, and stops counting callbacks, within a period.
    - `unplug(Loss::Reported)`: the same, and their `lost()` becomes true.
    - `plug_in()` makes it present again. Backends that stopped stay stopped, as a real device's stream does: the device must be opened again.

    A paced backend renders only while its device is present and hasn't been unplugged since the backend was opened. `present()` says whether it's plugged in.

### Device sources

18. **`DeviceSource`** is how the output controller reaches the device:
    - `available()` says whether the device is there to open;
    - `open(core, driver)` opens it, not started, to render as `driver`, or returns a `BackendError`.

    Step 2c implements it with miniaudio.
19. **`PacedDeviceSource(device, config)`** opens paced backends on a `SimulatedDevice`. `available()` is the device's `present()`. `open()` opens a paced backend with `config`, on that device, as the given driver, and returns `OpenFailed` if the device isn't present.

### The output controller

20. **`OutputController(core, source, config)`** keeps the core rendering through device loss. `OutputConfig` holds:

    | Setting | Default |
    |---|---|
    | Stall timeout | 100 ms |
    | Retry interval | 500 ms |
    | Freewheel period | 256 frames |
    | Freewheel format if no device has ever opened | 48 kHz, 2 channels |

    It's used from one thread, the control thread, which calls `poll()` at least every 10 ms. Each driver it opens gets a new `DriverId`, never `kDefaultDriver` and never one used before.
21. **`state()`** is `Stopped`, `Playing`, `Freewheeling`, or `Attaching`.
22. **`start()`** opens the device through the source, makes it the active driver, and starts it: `Playing`. If the device can't be opened or started, it starts freewheeling instead (requirement 24): `Freewheeling`.
23. **While `Playing`,** the device is lost when its `lost()` is true, or when the core's frame count hasn't moved for the stall timeout.
24. **Freewheeling after a loss:**
    1. It opens the freewheel driver: a paced backend that asks for real-time priority and doesn't record. It uses the lost device's sample rate and channel count, and the configured period.
    2. The freewheel driver's start time is the later of the core's `rendered_until()` and the moment the lost device was started. It therefore catches up the time it took to notice the loss. When `start()` freewheels because there's no device, the start time is the moment `start()` was called.
    3. It makes the freewheel driver active and starts it.
    4. Only then does it stop and close the lost device.

    State: `Freewheeling`.
25. **While `Freewheeling`,** once every retry interval, if the source is available, it opens the device and starts it, without making it active: `Attaching`. If opening or starting fails, it tries again after the next interval.
26. **While `Attaching`:**
    - **The device calls back:** it makes the device active, then stops and closes the freewheel driver: `Playing`, with the stall timer restarted.
    - **The device reports a loss, or doesn't call back within the stall timeout:** it closes the device, and the freewheel driver keeps going: `Freewheeling`.
27. **`stop()`** stops and closes whatever is running: `Stopped`. Destroying the controller stops it.

## Acceptance criteria

The tests are in `tests/acceptance/device-loss/`. Tests that wait for something fail, rather than hang, when it hasn't happened within the stated time. Times are measured with the steady clock; "within 50 ms of real time" means the frame count differs from the elapsed time × sample rate by at most 50 ms of frames.

The active driver (`active_driver_test.cpp`):

- [ ] `[auto]` `kDefaultDriver` is active at first. After `activate(a)`, blocks from `a` render, and blocks from any other driver are skipped and silenced. `active_driver()` returns `a`. (test: "Active driver: only the active driver renders")
- [ ] `[auto]` `resumed` is set on the first block rendered, and on the first block from each newly active driver, and on no other block. A skipped block in between doesn't change that, and a value set by the driver is ignored. (test: "Active driver: the first block after a handover is resumed")
- [ ] `[auto]` `rendered_until()` is `time_point{}` before anything renders, then the end of the last rendered block. A skipped block doesn't change it. (test: "Active driver: rendered_until is where the last block ended")
- [ ] `[auto]` Two threads render as two drivers while the main thread switches the active driver between them 1,000 times. The graph never runs on both at once, `frames_rendered()` equals what the graph rendered, and no more blocks are marked `resumed` than there were switches, plus one. (test: "Active driver: handovers under load")

The paced backend and simulated devices (`paced_backend_test.cpp`):

- [ ] `[auto]` At 48 kHz with a 1,024-frame period, over half a second:
  - at the end, the frame count is within 50 ms of real time, or gets there within the next 100 ms (a busy test runner can wake the paced thread late, and a backend that runs slow or fast never gets there);
  - blocks are at most `kMaxBlockFrames`;
  - each block's host time is the first block's plus `frames_to_duration()` of the whole periods before it, plus `frames_to_duration()` of the frames before it in its period (requirement 12, and render-core requirement 17);
  - the first block's host time falls between just before and just after `start()`;
  - `callbacks()` × the period equals `frames_rendered()`.

  (test: "Paced: renders in real time")
- [ ] `[auto]` A backend recording 4,800 frames holds exactly those frames, interleaved, matching what the graph rendered, after rendering more than that. (test: "Paced: records what it plays")
- [ ] `[auto]` With a start time 200 ms in the past, the first block's host time is that start time, and within 100 ms the frame count has caught up with real time. (test: "Paced: catches up from a start time in the past")
- [ ] `[auto]` After `stop()`, nothing renders for 100 ms, and starting again resumes rendering. Each kind of invalid configuration returns the matching error. (test: "Paced: stop, start, and invalid configurations")
- [ ] `[auto]` A backend that asks for real-time priority reports `Granted` or `Refused` once started; one that doesn't reports `NotRequested`. (test: "Paced: real-time priority is reported")
- [ ] `[auto]` Unplugging a simulated device stops the backend playing on it within 50 ms. After a silent loss `lost()` stays false; after a reported one it's true. Plugging the device back in doesn't restart that backend, but a backend opened on it afterwards renders. (test: "Simulated device: unplugging stops the backends on it")

The output controller (`output_controller_test.cpp`), polling every 5 ms:

- [ ] `[auto]` A silent loss, with a 200 ms retry interval:
  - **The sequence:** the device plays for 300 ms, then is unplugged without a report. The controller is `Freewheeling` within 1 s. 300 ms later the device is plugged back in, and the controller is `Playing` again within 2 s. It runs for another 300 ms, then stops.
  - **The clock** stays within 50 ms of real time from `start()` to `stop()`.
  - **The position:** every block the graph rendered is within 50 ms of where real time puts it.
  - **Fade-in marks:** exactly three blocks are marked `resumed`: the first, the first freewheel block, and the first block from the returned device.
  - **After `stop()`,** nothing renders for 100 ms, and the state is `Stopped`.

  (test: "Output: a silent device loss keeps the clock running")
- [ ] `[auto]` With a 10 s stall timeout, a reported loss still has the controller `Freewheeling` within 1 s. (test: "Output: a reported loss is noticed at once")
- [ ] `[auto]` With the device unplugged at `start()`, the controller freewheels at the default format and the frame count keeps up with real time. Once the device is plugged in, it's `Playing` within 2 s. (test: "Output: with no device at start, it freewheels until one appears")

Real-time safety (`realtime_test.cpp`):

- [ ] `[auto]` `activate()`, `active_driver()`, `rendered_until()`, `try_render()` with a driver, and `render_interleaved()` with a driver are called from a `KS_NONBLOCKING` function, so function effect analysis checks them at compile time. Under the `rtsan` preset, RealtimeSanitizer checks the paced backend's periods at run time in the tests above. (test: "Device-loss render path is real-time safe")

Everywhere:

- [ ] `[auto]` All of the above pass in every CI configuration: every preset on Linux, and `debug`, `release`, `asan`, and `tsan` on macOS arm64.

## Out of scope

- miniaudio's notifications, recognising a device when it returns, reopening it, and choosing a device (step 2c, with the harness in step 7). The cable-pull test (step 8).
- The fade-in itself (the output safety stage, step 3).
- A fallback device (ADR 0002 §4; after Phase 1).
- A device that returns with a different sample rate or channel count.
- Reporting a refused real-time priority through the engine's health checks; `priority()` only exposes it.
- The engine's control thread itself (Phase 1b).

## Open questions

None.
