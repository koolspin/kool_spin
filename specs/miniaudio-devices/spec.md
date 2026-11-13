# Feature: miniaudio devices

- **Status:** Approved by the maintainer on 2026-11-12. Protected: build loops can't change it.
- **Roadmap:** Phase 1a (step 7a, which finishes step 2c)
- **Built by:** interactive

## Goal

Play through a real device the way the device-loss work (2b) plays through simulated ones. Choose a backend and a device, and notice when that device goes. Recognise the same device when it comes back, and reopen it. Give every block an estimate of the device's output latency. The development harness (7b) and the cable-pull test (step 8) build on this.

## Context

- **Decisions that apply:**
  - ADR 0003 §15, device loss:
    - the control thread starts the freewheel driver when miniaudio reports that the device stopped, or when callbacks stop for 100 ms without a report;
    - it polls the device list for the device's return, because miniaudio doesn't report arrivals;
    - it reopens the device.

    Device threads get whatever priority their backend gives them.
  - ADR 0002 §4: device loss is normal operation, and a fallback device comes after Phase 1.
  - ADR 0003 §3 and the state-out spec: a block's `output_latency` is the driver's estimate. The miniaudio estimate was left for this step.
- **The maintainer's design choices** (2026-11-12):
  - Step 7 is two pull requests: this one, built interactively like the other drivers, then the harness, built by a loop.
  - A source that is asked for "the default device" resolves it to a concrete device when it's created. It then opens that device by its ID and waits for that device to return. It doesn't follow the system default. Opening a device by ID also stops PulseAudio (and so PipeWire) from moving the stream to another sink, so a pulled cable reaches the engine as a loss rather than as music moving to the laptop speakers.
- **What exists:**
  - `open_miniaudio()` opens the system default device, on miniaudio's default backends or its null backend. Its `lost()` is always false.
  - `DeviceSource` (`available()`, `open()`), `OutputController` (play, freewheel on loss, attach, hand back), and `PacedDeviceSource` for simulated devices (device-loss spec).
  - `render_interleaved()` takes an `output_latency` and gives it to every block; the miniaudio backend passes 0.
  - `ks-tone` lists devices and plays on one by index with its own miniaudio code. It stays standalone.
- **How miniaudio 0.11.26 reports a device going** (read from its source; the manual criteria check the real behaviour):
  - **Its notifications:** started, stopped, rerouted, interruption began and ended, and unlocked.
  - **When "stopped" is sent:**
    - backends that run their own audio thread (ALSA, the null backend) send it when the device stops, including when the application stops it, and before `ma_device_stop()` returns;
    - CoreAudio sends it when the device stops other than by `ma_device_stop()`;
    - PulseAudio sends it when the stream is suspended, and nothing when its sink disappears.
  - **When nothing is sent:** an unplugged ALSA device may stop without a notification. The output controller's stall timeout catches the silent cases.
  - **Underruns:** miniaudio doesn't report them, so this step doesn't either.
- **Tests run on miniaudio's null backend**, which has one device and calls back in real time. Hardware behaviour is checked by hand.

## Requirements

### Library

1. Everything is in `ks::engine`, in `<ks/engine/miniaudio_backend.h>`. Existing uses of `open_miniaudio()`, `MiniaudioConfig`, and `MiniaudioBackends` keep compiling and behaving as before.

### Backends

2. **`MiniaudioBackends`** gains `PulseAudio`, `Alsa`, `Jack`, and `CoreAudio`, after `System` and `Null`. `System` tries miniaudio's default order. Each other value uses only that backend. A backend that this platform or build lacks fails with `NoBackend` when it's used.
3. **`backend_name()`** gives each value's name: `"system"`, `"null"`, `"pulseaudio"`, `"alsa"`, `"jack"`, and `"coreaudio"`. **`parse_backend()`** is its inverse. It's case-sensitive and returns nothing for any other string.

### Devices

4. **`DeviceKey`** identifies a device to the backend that listed it. It holds the bytes of miniaudio's device ID: `kDeviceKeySize`, 256 bytes. Keys compare equal when their bytes do. A value-initialized key is all zeros.
5. **`list_output_devices(backends)`** returns an `OutputDeviceList`:
   - `backend`: miniaudio's name for the backend it used, for example `"PulseAudio"` or `"Null"`;
   - `devices`: each playback device's `name`, `key`, and `is_default`, in miniaudio's order.

   It returns `NoBackend` if no backend could be initialized, and `OpenFailed` if the devices couldn't be listed.

### Opening a device

6. **`MiniaudioConfig::device`** (`std::optional<DeviceKey>`, last) chooses the device:
   - **With a key,** `open_miniaudio()` opens that device of the chosen backends, by ID. If no device has that key, it returns `OpenFailed`.
   - **Without a key,** it opens the system default device, as before.
7. **`StreamInfo`** gains:
   - `periods`: the device's buffer, in periods;
   - `output_latency`: `frames_to_duration(period_frames × periods, sample_rate)`, the backend's estimate of the time from a block's host time to its first frame leaving the device.
8. Every block the miniaudio backend renders carries `info().output_latency` as its `output_latency`.

### Loss

9. **`DeviceEvent`** names miniaudio's notifications: `Started`, `Stopped`, `Rerouted`, `InterruptionBegan`, `InterruptionEnded`, and `Unlocked`.
10. **`LossDetector`** decides from those events whether the device has gone:
    - `on_event(event)`: the device is lost on `Stopped` unless a stop is in progress, and on `Rerouted` or `InterruptionBegan`. Other events change nothing.
    - `stopping()` marks a stop as in progress, and `starting()` ends it.
    - `lost()` says whether the device has been lost. Once lost, it stays lost: the device must be opened again.

    A detector starts not lost, with no stop in progress. Every method is `noexcept KS_NONBLOCKING`, and any thread may call any of them, because miniaudio can notify from the audio thread.
11. **The miniaudio backend** feeds every miniaudio notification to its own `LossDetector`. Its `stop()` calls `stopping()` before stopping the device, and its `start()` calls `starting()` before starting it. Its `lost()` is the detector's.

### The device source

12. **`MiniaudioDeviceSource::create(config)`** returns a `MiniaudioDeviceSource`, which is a `DeviceSource`. `MiniaudioSourceConfig` holds the backends, an optional device key, and the channel count, sample rate, and period, each 0 for the device's or backend's choice. Creating the source:
    - initializes the backend, or returns `NoBackend`;
    - **resolves the device:**
      - the key if one is given, whether or not a device has it now;
      - otherwise the listed device marked default, or the first device if none is marked;
      - with no key and no devices, it returns `OpenFailed`.

    `device()` returns the resolved device. Its name is empty when a given key isn't listed.
13. **`available()`** lists the backend's devices, through one miniaudio context that the source keeps, and says whether one has the resolved key. If listing fails, the device isn't available.
14. **`open(core, driver)`:**
    - It returns `OpenFailed`, and opens nothing, if the device isn't available.
    - Otherwise it opens the device by its key, not started, with the source's backends and period, rendering as `driver`.
    - **The format:** until a device has opened through this source, it asks for the configured sample rate and channel count. After that, it asks for the sample rate and channel count that the first device opened with, so a device that returns comes back in the same format. If the device's own format has changed, miniaudio converts.
15. Backends that the source opens don't depend on the source, and may outlive it.
16. **The output controller** plays through a `MiniaudioDeviceSource` as it does through any `DeviceSource`.

### Real time

17. The backend's callback, its notification handler, and every `LossDetector` method are `noexcept KS_NONBLOCKING`. Listing, creating a source, `available()`, and `open()` run on the control thread.

## Acceptance criteria

The tests are in `tests/acceptance/miniaudio-devices/`, on miniaudio's null backend unless a criterion says otherwise. Tests that wait for something fail, rather than hang, when it hasn't happened within the stated time.

- [ ] `[auto]` **Backend names:** every `MiniaudioBackends` value round-trips through `backend_name()` and `parse_backend()`; `"PulseAudio"`, `""`, and `"wasapi"` parse to nothing. (test: "Miniaudio: backends by name")
- [ ] `[auto]` **Listing:** the null backend lists exactly one device, marked default, named `"NULL Playback Device"`, with backend `"Null"`; listing again gives the same key. (test: "Miniaudio: the null backend's devices")
- [ ] `[auto]` **Opening by key:** `open_miniaudio()` with the null device's key opens it, and it renders once started; with a key that differs in one byte, it returns `OpenFailed`. (test: "Miniaudio: opening a device by key")
- [ ] `[auto]` **Latency:** with a 256-frame period, `info()` reports at least one period, and an `output_latency` above 0 that equals `frames_to_duration(period_frames × periods, sample_rate)`. Every block a recording graph gets over 200 ms carries that latency. (test: "Miniaudio: blocks carry the output latency")
- [ ] `[auto]` **Loss detector:**
  - a new detector isn't lost;
  - `Stopped` makes it lost, but not while a stop is in progress;
  - `Stopped` after `starting()` makes it lost again;
  - `Rerouted` and `InterruptionBegan` make it lost, even during a stop;
  - `Started`, `InterruptionEnded`, and `Unlocked` don't;
  - once lost, no event or call makes it not lost.

  (test: "Loss detector: which events mean the device is lost")
- [ ] `[auto]` **Stopping isn't a loss:** a null device started, stopped, started again, and stopped, about 100 ms in each state, never reports `lost()`, and its callbacks count up again after the restart. (test: "Miniaudio: stopping the device isn't a loss")
- [ ] `[auto]` **The source:**
  - created with no key, it resolves the null device;
  - created with that device's key, it resolves the same device, with its name;
  - with a key that differs in one byte, creation succeeds, `device()` has that key and an empty name, `available()` is false, and `open()` returns `OpenFailed`;
  - with the null device, `available()` is true, and `open()` gives a backend that isn't started, renders as the given driver once started, and reports the configured period;
  - a second `open()` reports the first one's sample rate and channel count.

  (test: "Miniaudio source: resolving and opening the device")
- [ ] `[auto]` **The output controller on a real backend:** with a source on the null device, `start()` is `Playing`. Over 500 ms the frame count is within 50 ms of real time, or gets there within the next 100 ms. After `stop()` the state is `Stopped`, and nothing renders for 100 ms. (test: "Output: playing through a miniaudio device")
- [ ] `[auto]` **Real time:** `LossDetector`'s methods are called from a `KS_NONBLOCKING` function, so function-effect analysis checks them, and the tests above run clean under `rtsan`. (test: "miniaudio devices are real-time safe")
- [ ] `[manual]` **Cable pull on Linux:** the hidden test "Cable: a tone through a chosen device" lists the devices, then plays a tone through `OutputController` and a `MiniaudioDeviceSource` for `KS_CABLE_SECONDS` (default 60), printing each state change with its time. It uses the backend in `KS_CABLE_BACKEND` (default `system`) and device number `KS_CABLE_DEVICE` from the list (default: the default device). On the Inpulse 500 through PipeWire's PulseAudio backend:
  - pulling the USB cable makes the controller `Freewheeling` within 1 s, and the tone doesn't move to the laptop's speakers;
  - plugging it back in makes it `Playing` within 2 s, with the tone back on the Inpulse.
- [ ] `[manual]` **Cable pull on macOS:** the same, with CoreAudio.
- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux and on macOS arm64 (`debug`, `release`, `asan`, `tsan`).

## Out of scope

- The development harness (7b), and the one-hour soak test and cable pull through it (step 8).
- Underruns, which miniaudio doesn't report.
- A fallback device, and following the system default device (after Phase 1).
- Recognising a device whose ID changes when it returns. For example, if ALSA renumbers the card, the source waits for the old ID.
- Hot-plugging a different device, and choosing a device by name.
- Windows backends.
- Moving `ks-tone` onto these functions.

## Open questions

None.
