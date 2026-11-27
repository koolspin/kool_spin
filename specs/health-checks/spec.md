# Feature: Health checks and keeping the machine awake (steps 10a and 10b)

- **Status:** Approved by the maintainer on 2026-11-27. Protected: build loops can't change it.
- **Roadmap:** Phase 1b, steps 10a and 10b
- **Built by:** interactive

## Goal

The engine checks what it can't control but can see: whether its audio thread runs at real-time priority, whether its memory and its tracks are locked, whether the CPU is in a power-saving profile or the laptop runs on battery, and whether render load comes close to the deadline. It reports each problem in its log and in shared state, which `ks-play health` prints. While a deck is loaded, the machine doesn't go to sleep, even if the supervisor crashes.

## Context

- **ADRs that apply:**
  - ADR 0002 §7 (the health checks, and blocking sleep while a deck is loaded); §1 (the engine's and the supervisor's dependencies); §3 (track memory locked when the OS permits).
  - ADR 0003 §6 (the engine's control thread runs the health checks); §9 (memory locking: `mlockall(MCL_CURRENT)` after starting and after each device opens, tracks in chunks as they decode, never `MCL_FUTURE`, best effort, and the health check warns); §15 (no rtkit; device threads get their backend's priority; the freewheel thread asks for `SCHED_FIFO` or a time-constraint policy; the health check reports a thread that doesn't get the priority it asked for).
- **What exists:**
  - **The freewheel and paced threads** ask for real-time priority (`SCHED_FIFO` 70 on Linux, a time-constraint policy on macOS) and record whether they got it (`PacedBackend::priority()`). Nothing reports it.
  - **Device threads** run at whatever their backend gives them. On Linux, miniaudio's own threads (the PulseAudio, ALSA, and null backends) run at `SCHED_OTHER`; JACK's are real-time. On macOS, Core Audio's I/O thread is real-time.
  - **Track memory** is faulted in by the control thread as it decodes, before the audio thread reads it (`SegmentTrack::pass()`). Nothing is locked: the decode-worker spec left `mlock` to this step.
  - **Render load:** `RenderStats` reaches `Engine::rendered()` after every block. The status block holds the last block's load and the highest ever.
  - **`ks-play status`** prints a fixed set of 11+ lines, which its tests check exactly; this spec adds a command rather than a line.
  - **Nothing blocks sleep.**
- **The maintainer's design choices** (2026-11-27):
  - Two steps under one spec, each its own pull request: **10a**, the health checks and memory locking (the engine); **10b**, keeping the machine awake (the supervisor).
  - **Linux:** the supervisor runs `systemd-inhibit` around a process that waits on a pipe the engine also holds, so the block lasts while either is alive. No new library.
  - **macOS:** `caffeinate -i -d`, a stock macOS tool, on the same pipe as Linux, for idle system sleep and display sleep, so the block outlives a supervisor crash there too. No framework in the supervisor. App Nap waits for the Qt UI (Phase 3), the first bundled app: it applies to apps, and needs Objective-C.
  - **Reporting:** a new seqlocked health block in the engine-state segment (layout version 3); `ks-play` prints it; the engine logs a warning when a check starts failing.
- **The two steps:**
  - **10a:** requirements 1 to 20, and 27's parts for 10a; the criteria under "Health checks".
  - **10b:** requirements 21 to 26, and 27's parts for 10b; the criteria under "Keeping awake", and the manual ones.
- **This spec amends:**
  - `specs/state-out/spec.md` and `specs/resume-state/spec.md`: the engine-state segment gains a health block, after the resume slots, and its layout version becomes 3. `tests/acceptance/resume-state/layout_test.cpp` and `tests/acceptance/state-out/layout_test.cpp` check version 2 today; that line changes to 3 in both, which this spec approves.
  - `specs/ks-play/spec.md` requirement 22's command table: a `health` command (requirement 19).
  - `specs/processes/spec.md`: `ks-engine`'s command line and start (requirements 8 and 9), and `kool_spin`'s command line and start (requirements 13 and 15).
  - Everything else in them still applies.
- **Terms:** a *check* is one of five: real-time priority, memory locking, power profile, power source, render load. A check *fails* when it finds a problem; it's *unknown* when it can't tell, which isn't a failure.

## Requirements

### 10a: memory locking

1. **The process (Linux):** the control thread calls `mlockall(MCL_CURRENT | MCL_ONFAULT)` once it has built the engine, and again after each device opens. `MCL_ONFAULT` locks pages only once they're touched, so the track segments' unused space, sized at the cap, is never committed. It never uses `MCL_FUTURE`. Whether the last call succeeded, and its `errno` if not, is recorded. On macOS, which has no `mlockall()`, nothing is done at process level, and the process part of the memory check is *unknown*.
2. **Tracks:** each time `SegmentTrack::pass()` faults in a newly decoded range, it then calls `mlock()` on that range, widened to whole pages. A failure (`ENOMEM`, `EAGAIN`, `EPERM`) adds the range's bytes to the track's *unlocked* count, and the track plays on; the range isn't retried. The bytes locked and unlocked are counted per track and summed over the tracks the engine holds. Unmapping a track unlocks it.
3. **The memory check fails** when the last `mlockall()` failed (Linux), or any held track has unlocked bytes. It records `RLIMIT_MEMLOCK`'s soft limit (`unlimited` as `UINT64_MAX`).

### 10a: real-time priority

4. **The device thread:** at its first callback after each device starts, the miniaudio backend stores the calling thread's `pthread_t` in an atomic, through a new `core::current_thread()` in `<ks/core/thread.h>`. `pthread_self()` makes no system call on Linux or macOS, but isn't annotated, so `current_thread()` suppresses `-Wfunction-effects` as `core::monotonic_now()` does. **This is a new suppression, which this spec approves;** RealtimeSanitizer still checks the call at run time.
5. **Raising it:** on the control thread's next pass after a new handle appears, it reads the thread's scheduling.
   - **Linux:** if the policy isn't `SCHED_FIFO` or `SCHED_RR`, it asks for `SCHED_FIFO` at 70 with `pthread_setschedparam()`, as the freewheel thread does, and reads the policy again.
   - **macOS:** if the thread doesn't have a time-constraint policy (`thread_policy_get()` with `THREAD_TIME_CONSTRAINT_POLICY` reports the default), it sets one from the device's period, as the paced backend does, and reads it again.
   - A thread that already has real-time priority (JACK's, Core Audio's) isn't touched.
   - **The handle's lifetime:** only the control thread uses it, and it clears it before it closes the device, so it never names a thread that has ended.
6. **The real-time check** looks at the active driver's thread: the device thread (requirement 5), or the freewheel thread while it's active (`PacedBackend::priority()`). It fails when that thread doesn't have real-time priority after asking. It records the policy (`other`, `fifo`, `rr`, `time-constraint`, or `unknown`), the priority (Linux), and, on Linux, `RLIMIT_RTPRIO`'s soft limit.

### 10a: power

7. **Power profile (Linux),** read from under a root, `/sys` unless the test-only `--sysfs-root <dir>` is given (requirement 17). The check fails when any of these holds:
   - `firmware/acpi/platform_profile` is `low-power`, `quiet`, or `cool`;
   - any `devices/system/cpu/cpu<n>/cpufreq/energy_performance_preference` is `power`;
   - any `devices/system/cpu/cpu<n>/cpufreq/scaling_governor` is `powersave`, and that CPU's `scaling_driver` is neither `intel_pstate` nor `amd-pstate-epp`, whose `powersave` is their normal, dynamic governor.

   It records which file failed it, and the value. It's *unknown* when none of these files can be read. On macOS it's always *unknown*: Low Power Mode is read through Foundation, which waits for the Qt UI, as App Nap does.
8. **Power source:**
   - **Linux,** from `class/power_supply/*` under the same root: *battery* when some supply's `type` is `Battery` and its `status` is `Discharging`, and no supply whose `type` is `Mains` or `USB` has `online` 1; *mains* when some `Mains` or `USB` supply has `online` 1; otherwise *unknown* (a desktop with no supplies listed).
   - **macOS:** `IOPSGetProvidingPowerSourceType()`: *battery* for `kIOPMBatteryPowerKey`, *mains* for `kIOPMACPowerKey`, *unknown* otherwise. This links IOKit and CoreFoundation into `ks-engine` on macOS, which ADR 0002 records (requirement 27).
   - The check fails on *battery*.

### 10a: render load

9. **`Engine::take_peak_load()`:** the highest render load of any block since the last call, or since the engine was built; the call resets it to 0. The audio thread updates it in `rendered()` with a lock-free maximum (a relaxed compare-and-swap loop); the control thread takes it with an exchange. Both are `KS_NONBLOCKING`.
10. **The load check fails** while the highest load taken in the last 10 s is 0.7 or more, so a burst is reported and then clears 10 s after it ends. It records that highest load.

### 10a: the health pass

11. **When:** the control thread runs a health pass 100 ms after `Ready`, then once a second. Each pass runs every check, from what it has recorded and what it reads then; the power checks read at most 64 files. A pass blocks nothing on the audio thread.
12. **The health block** (`ipc::HealthState`, in `<ks/ipc/state.h>`), a `Seqlock<HealthState>` added to `EngineState` after the resume slots, written only by the control thread, after each pass. Every field is a `std::uint64_t`:

    | Field | Holds |
    |---|---|
    | `passes` | Health passes so far; 0 before the first |
    | `host_time` | The steady clock's time of the pass, in nanoseconds |
    | `known` | One bit per check that isn't *unknown*: `kHealthRealtime` (1), `kHealthMemory` (2), `kHealthPowerProfile` (4), `kHealthPowerSource` (8), `kHealthLoad` (16); 10b adds `kHealthAwake` (32) |
    | `failing` | One bit per check that fails, with the same values; always within `known` |
    | `policy` | Requirement 6's policy: 0 unknown, 1 other, 2 fifo, 3 rr, 4 time-constraint |
    | `priority` | The thread's priority (Linux), or 0 |
    | `rtprio_limit` | `RLIMIT_RTPRIO`'s soft limit (Linux), or 0 |
    | `memlock_limit` | `RLIMIT_MEMLOCK`'s soft limit; `UINT64_MAX` for unlimited |
    | `process_lock` | 0 not tried or not available, 1 locked, 2 failed |
    | `locked_bytes`, `unlocked_bytes` | Requirement 2's sums |
    | `power_profile` | 0 unknown, 1 normal, 2 saving |
    | `power_source` | 0 unknown, 1 mains, 2 battery |
    | `peak_load` | Real: requirement 10's highest load |
    | `awake` | 10b: 0 unknown, 1 blocked, 2 not blocked |

    `kEngineStateLayoutVersion` becomes 3. The segment's size grows by the block's; nothing before it moves. The same `static_assert`s as the other blocks pin its size and offsets.
13. **Log lines:** after each pass, for each check whose failing bit changed, the engine writes one line, as it writes its other reports:
    - **starts failing,** at `warning`:
      - `health: the audio thread isn't real-time (<policy>); RLIMIT_RTPRIO is <n>, and 70 is needed`;
      - `health: memory isn't locked (<what failed>); RLIMIT_MEMLOCK is <limit>`;
      - `health: the CPU is in a power-saving profile (<file> is <value>)`;
      - `health: running on battery`;
      - `health: render load reached <load> in the last 10 s`;
    - **stops failing,** at `info`: `health: <check> is fine again`, with the check's name: `real-time priority`, `memory locking`, `power profile`, `power source`, `render load`.

    The first pass logs a line for each check that fails then. A check that keeps failing isn't logged again.

### 10a: clients

14. **`client::HealthInfo`** in `ks::client`, from the health block, read through its seqlock like the other blocks, with each field decoded and validated (an unknown code reads as unknown; `failing` outside `known` is dropped). `client::Snapshot` gains `health`. `client::State::health()` returns it, or nothing while `passes` is 0.
15. **Restarts:** a restarted engine starts its health block again from 0 passes; the block isn't part of the resume state.

### 10a: options

16. **`ks-engine`'s output options** don't change.
17. **`--sysfs-root <dir>`,** for tests, accepted by `ks-engine` on Linux only (a usage error on macOS), and given through `kool_spin`'s `--`. Not in `--help`. Absolute, at most 4,096 bytes.

### 10a: `ks-play`

18. **`ks-play`'s exit statuses** don't change.
19. **`ks-play health`** waits up to 2 s for the first health pass, then prints one line and exits with status 0:

    `health failing=<names> realtime=<0|1|unknown> policy=<policy> priority=<n> rtprio_limit=<n> memory=<0|1|unknown> process_lock=<none|locked|failed> memlock_limit=<n|unlimited> locked_bytes=<n> unlocked_bytes=<n> power_profile=<unknown|normal|saving> power_source=<unknown|mains|battery> peak_load=<x> awake=<unknown|blocked|not-blocked>`

    `failing` is a comma-separated list of the failing checks' names (`realtime`, `memory`, `power-profile`, `power-source`, `load`, `awake`), or `none`. `realtime` and `memory` are 1 when the check passes, 0 when it fails. Reals use `%.6g`. Without a pass in 2 s, it exits with status 1 and `ks-play: no health check yet`. `--help` lists it.
20. **`ks-play status`** doesn't change.

### 10b: keeping the machine awake

21. **The keep-awake pipe:** at launch, before it starts the first engine, `kool_spin` creates a pipe (both ends close-on-exec). It keeps both ends for as long as it runs, and gives every engine it starts the write end as `--keep-awake-fd <n>`. The engine keeps it open and never reads or writes it. When the supervisor crashes, the engine still holds it.
22. **When:** the supervisor keeps the machine awake while any deck is loaded, by the engine's deck state blocks (which it already reads for the journal) or by a load it has sent. It lets the machine sleep 5 s after no deck is loaded, so that swapping a track doesn't release and take it again. While an engine restarts, it keeps what it had.
23. **The inhibitor:** to keep awake, the supervisor starts, in a process group of its own, with `posix_spawn`:
    - **Linux:** `systemd-inhibit --what=sleep:idle --who=kool_spin --why="A deck is loaded" --mode=block cat`;
    - **macOS:** `/usr/bin/caffeinate -i -d cat` (`-i` idle system sleep, `-d` display sleep);

    with the pipe's read end as standard input, `/dev/null` as standard output, and its standard error to the supervisor's log. `systemd-inhibit` is found on `PATH`. `cat` reads until every write end has closed, and the tool holds the block while `cat` runs. To let the machine sleep, the supervisor sends `SIGTERM` to the process group and reaps it. If the supervisor crashes, `cat` waits until the engine (the orphan) exits too, so sleep stays blocked while the engine plays; then the tool exits and the block goes. The test-only `kool_spin --inhibit-command <path>` runs `<path>` in place of the tool, with the same arguments after it; not in `--help`.
24. **Nothing is linked:** the supervisor runs a system tool, so its dependencies stay the standard library (ADR 0002 §1 records the tools, requirement 27).
25. **Failures:** if the tool can't be started, or exits while the supervisor wants it running, the supervisor logs one warning, with its exit status and the start of its standard error (for example, polkit refusing), and tries again every 30 s while a deck is loaded. A success after a warning logs one info line. Playback carries on.
26. **Reporting:** the supervisor sends the engine a new message, `KeepAwake` (type 29, supervisor to engine, request ID 0, body `state: u8`: 1 blocked, 2 not blocked), whenever its state changes and to each engine after `Ready`. The engine records it in the health block's `awake`, and `kHealthAwake` fails while a deck is loaded and the state is *not blocked*. It's *unknown* until the first `KeepAwake`. `decode()` refuses another `state`, and a non-zero request ID.

### Documents

27. **ADRs and docs,** which this spec approves:
    - **10a:**
      - ADR 0003 §9: `mlockall(MCL_CURRENT | MCL_ONFAULT)` on Linux, and why (the track segments' unused space); macOS has no `mlockall()`, so only tracks are locked there.
      - ADR 0003 §15: the control thread raises a device thread that isn't real-time, as the freewheel thread raises itself, still without rtkit.
      - ADR 0003 §6: the health pass and the health block.
      - ADR 0002 §1: `ks-engine` links the IOKit and CoreFoundation system frameworks on macOS, for the power source.
      - ADR 0002 §7: App Nap and Low Power Mode wait for the Qt UI.
    - **10b:** ADR 0002 §1: `kool_spin` runs `systemd-inhibit` on Linux and `caffeinate` on macOS, and links nothing new; §7: how sleep is blocked, and that the block lasts while the engine plays, even after a supervisor crash.
    - Each step's PR updates the roadmap and `CLAUDE.md`'s Status.

## Acceptance criteria

The tests are in `tests/acceptance/health-checks/`. They follow `specs/processes/spec.md`'s rules for tests: separate runtime directories, the null backend, and no process outliving its test. Tests that start `kool_spin` can't run in the build-loop sandbox (`CLAUDE.md`). Limits are set with `setrlimit()` in the child before it executes `kool_spin`, so the engine inherits them.

Health checks (10a; `health_test.cpp` and `health_process_test.cpp`):

- [ ] `[auto]` **The block:** `HealthState`'s size and every field's offset; `kEngineStateLayoutVersion` is 3; the health block comes after the resume slots, and everything before it is where it was. (test: "Health: the block's layout")
- [ ] `[auto]` **Peak load:** an engine whose `rendered()` gets loads 0.2, 0.9, 0.4: `take_peak_load()` returns 0.9, then 0; after one more block at 0.3, 0.3. From two threads, 100,000 random loads, every value is taken once at most and the highest is never lost. (test: "Health: peak load")
- [ ] `[auto]` **The load check:** with the health pass's clock under the test's control, a peak of 0.75 fails the check and logs `render load reached 0.75`; it still fails at 9.9 s; at 10 s with lower loads since, it passes and logs `render load is fine again`. A peak of 0.69 never fails it. (test: "Health: the load check over 10 s")
- [ ] `[auto]` **Power from a fake `/sys`** (Linux; skipped on macOS): with `--sysfs-root`, each of these fails the profile check and names its file: `platform_profile` `low-power`; one CPU's EPP `power`; `powersave` with `acpi-cpufreq`. These pass it: `powersave` with `intel_pstate`, and with `amd-pstate-epp`; `balanced`. An empty root is *unknown* for both checks, and logs nothing. A `Battery` `Discharging` with a `Mains` `online` 0 fails the source check; with `online` 1, or a `USB` supply `online` 1, it's *mains*. (test: "Health: power from a fake sysfs")
- [ ] `[auto]` **Changes are logged once:** with a fake root at `balanced`, changed to `low-power` while the engine runs, `ks-play health` shows `power_profile=saving` within 2 s and the log has one warning; after 5 more passes, still one; changed back, one info line `power profile is fine again`. (test: "Health: a change is logged once")
- [ ] `[auto]` **Real-time refused** (Linux; skipped on macOS): with `RLIMIT_RTPRIO` 0, soft and hard, the engine plays, `ks-play health` shows `realtime=0 policy=other rtprio_limit=0` and `realtime` in `failing`, and the log has the warning. (test: "Health: real-time priority refused")
- [ ] `[auto]` **Real-time granted:** on macOS always, and on Linux when the hard `RLIMIT_RTPRIO` is 70 or more (skipped otherwise), with the soft limit raised to 70: `realtime=1`, with `policy=time-constraint` (macOS) or `policy=fifo priority=70` (Linux); on Linux, `/proc/<pid>/task/*/stat` shows one engine thread at `SCHED_FIFO` 70. (test: "Health: real-time priority granted")
- [ ] `[auto]` **Memory refused** (Linux; skipped on macOS): with `RLIMIT_MEMLOCK` 64 KiB, soft and hard, a 10 s track loads, decodes, and plays: `process_lock=failed memlock_limit=65536`, `unlocked_bytes` at least the track's size, `memory` in `failing`, one warning. (test: "Health: memory locking refused")
- [ ] `[auto]` **Memory locked:** on Linux when the hard `RLIMIT_MEMLOCK` is unlimited (skipped otherwise), with the soft limit raised to it; on macOS always. A 10 s track loads and decodes: `locked_bytes` at least the track's size, `unlocked_bytes=0`; on Linux `process_lock=locked`, `VmLck` in `/proc/<pid>/status` at least the track's size, and `VmRSS` less than 256 MiB (the unused segment space isn't committed). (test: "Health: memory locked")
- [ ] `[auto]` **`ks-play health`:** every field in requirement 19's order; `failing=none` when nothing fails (a fake root at `balanced`, with mains), checked where the hard limits allow real-time priority 70 and unlimited memory locking; elsewhere, the power checks don't fail; exit status 0. Against an engine that never passes (the paced backend with `-- --hang-control-after 0`, so the control thread stops just after Ready, and the supervisor's `--ping-interval 60000`), status 1 and `ks-play: no health check yet`. `--help` lists `health`. (test: "Play: health")
- [ ] `[auto]` **After a restart:** after `kill -9` of the engine, the new engine's health block counts passes from 1 again, and its checks are logged afresh. (test: "Health: after an engine restart")
- [ ] `[auto]` **Options:** `--sysfs-root` twice, a relative path, and a 4,097-byte one are usage errors with status 2 (Linux); on macOS, `--sysfs-root` is one. `ks-engine --help` doesn't list it. (test: "Health: options")
- [ ] `[auto]` **RealtimeSanitizer:** under `rtsan`, every test above passes with no report, so `current_thread()` and `take_peak_load()` run cleanly in the callback.

Keeping awake (10b; `awake_test.cpp`):

- [ ] `[auto]` **While a deck is loaded:** with `--inhibit-command` a fake that records its arguments and process group and then runs `cat`, nothing runs with no deck loaded; within 1 s of a load it runs, with exactly requirement 23's arguments for the platform; `ks-play health` shows `awake=blocked`. 5 s after the unload, its process group is gone. An unload and a load 1 s apart leave the same process running. (test: "Awake: while a deck is loaded")
- [ ] `[auto]` **Outlives a supervisor crash:** with a deck loaded, after `kill -9` of `kool_spin`, the fake's `cat` keeps running while the engine plays; it exits within 1 s of the engine's `SIGTERM`. (test: "Awake: a supervisor crash keeps it")
- [ ] `[auto]` **An inhibitor that fails:** a fake that writes `Access denied` to standard error and exits with status 1 gives one warning with both, `awake=not-blocked` and `awake` in `failing`; playback carries on; it's retried 30 s later (the fake counts its runs). With `--inhibit-command` a path that doesn't exist, the same. (test: "Awake: an inhibitor that fails")
- [ ] `[auto]` **Stopping:** after `SIGTERM` to `kool_spin` with a deck loaded, no fake process is left once it has exited. (test: "Awake: stopping")
- [ ] `[auto]` **macOS assertions** (macOS, with the real `caffeinate`; skipped on Linux): with a deck loaded, `pmset -g assertions` lists `PreventUserIdleSystemSleep` and `PreventUserIdleDisplaySleep` held by a `caffeinate` process whose process group the supervisor started; 5 s after the unload, neither is listed for it. (test: "Awake: macOS assertions")
- [ ] `[auto]` **The message:** `KeepAwake` encodes and decodes; state 0 and 3, a request ID of 1, and a body a byte short or long are refused. (test: "Awake: the KeepAwake message")
- [ ] `[auto]` **Options:** `--inhibit-command` twice and a relative path are usage errors with status 2; `kool_spin --help` doesn't list it. (test: "Awake: options")

Everywhere:

- [ ] `[auto]` **The earlier steps' tests** all pass, unchanged except the two `layout_test.cpp` files' layout version (requirement 12).
- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux in every preset, and on macOS arm64 in `debug`, `release`, `asan`, and `tsan`.
- [ ] `[manual]` **Linux laptop:** with suspend after 2 minutes idle, a loaded deck keeps it awake for 5 minutes; 2 minutes after the unload, it suspends. `ks-play health` on the maintainer's laptop shows what it should: on battery, and with the CPU set to power-saving.
- [ ] `[manual]` **macOS:** the same with the Mac's sleep settings; `pmset -g assertions` shows both while a deck is loaded.
- [ ] `[manual]` **Real-time on the Inpulse 500:** with `RLIMIT_RTPRIO` raised (the `audio` group), through PulseAudio and through JACK, `ks-play health` shows `realtime=1`, and the device thread is `SCHED_FIFO` (raised by the engine through PulseAudio; JACK's own through JACK).

## Out of scope

- **rtkit,** and any D-Bus client in the engine or the supervisor (ADR 0003 §15).
- **Display sleep and screen locking on Linux:** desktops handle them through their own interfaces (`org.freedesktop.ScreenSaver`, GNOME's session manager), not logind's `idle` lock, which some honour and some don't. The Qt UI, which has D-Bus, can take them in Phase 3.
- **App Nap and Low Power Mode on macOS:** with the Qt UI (Phase 3).
- **Fixing what the checks find:** they report; the user raises limits or changes settings.
- **Health checks in the supervisor or the workers:** the engine is the process whose timing matters.
- **Refusing to play:** a failing check never stops the engine.
- **Locking the workers' or the supervisor's memory.**
- **Other operating systems:** Windows comes later.

## Open questions

None. The maintainer approved these on 2026-11-27, with the spec:

1. **Two steps:** 10a (health checks and memory locking, in the engine) and 10b (keeping awake, in the supervisor), each its own pull request, under this spec.
2. **Linux sleep** through `systemd-inhibit`, and **macOS** through `caffeinate`, each around a `cat` on a pipe the engine also holds, so the block lasts while the engine plays, even after a supervisor crash (requirements 21 and 23).
3. **Reporting** through a health block in the engine-state segment, layout version 3 (requirement 12), and a `ks-play health` command of its own, since `status`'s output is pinned by its tests (requirement 19).
4. **Raising the device thread** to `SCHED_FIFO` 70 or a time-constraint policy from the control thread, with a second function-effects suppression, for `pthread_self()` (requirements 4 and 5; ADR 0003 §15).
5. **The load threshold:** 0.7, over 10 s (requirement 10).
6. **A 5 s grace** before sleep is allowed again (requirement 22).
7. **App Nap and Low Power Mode** wait for the Qt UI (Phase 3).
