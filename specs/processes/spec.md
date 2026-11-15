# Feature: Supervisor and engine processes

- **Status:** Approved by the maintainer on 2026-11-15. Protected: build loops can't change it.
- **Roadmap:** Phase 1b, step 3a
- **Built by:** interactive

## Goal

Run the engine in its own process under a supervisor. `kool_spin` starts `ks-engine` and gives it the engine-state segment. It lets clients connect and watch the engine's state, and stops everything with a fade-out. This is the skeleton of the process model, and later steps build on it:

- step 3b adds the clients' command rings;
- step 4 adds the logs;
- step 5 adds tracks;
- step 7 adds restarts;
- step 8 adds recovery from a supervisor crash.

## Context

- **ADRs that apply:**
  - **ADR 0002 §1:** the supervisor depends on the standard library only.
  - **ADR 0003 §6:**
    - the supervisor is the hub, and the engine's only socket is to it;
    - children inherit one end of a `socketpair()`;
    - the supervisor is single-threaded: one `poll` loop, with a self-pipe for `SIGCHLD`.
  - **ADR 0003 §11:** the engine-status heartbeat counts rendered blocks.
  - **ADR 0003 §12:** each child holds a lock file, and the engine keeps playing when the supervisor dies.
  - **ADR 0003 §13:**
    - children are started with `posix_spawn`, each in its own process group, so Ctrl-C at a terminal reaches only the supervisor, which stops the others with a fade-out;
    - children don't ask to be killed when their parent dies.
- **What exists:**
  - **`ks::ipc`:**
    - segments: `SegmentFactory` and `Region::map()`;
    - the runtime directory and lock files;
    - the socket protocol: `socket_pair()`, `Connection`, `Listener`, `connect_to_supervisor()`, and the `Ping`, `Pong`, and `Error` messages.
  - **`ks::engine`:**
    - `Engine`, built on an `EngineState`, which publishes the state and the heartbeat after every block;
    - `RenderCore`;
    - `OutputController`: it plays through a device, freewheels while the device is lost, and its `stop()` fades out;
    - `MiniaudioDeviceSource`, and miniaudio's null backend, which calls back in real time without a device.
  - **The harness's device mode** puts these together in one process. `ks-engine` does the same in a process of its own.
- **The maintainer's calls for step 3 (2026-11-15):**
  - **Two parts:** step 3 is split into 3a (this spec) and 3b. Step 3b covers the clients' command rings and attaching and detaching them.
  - **The recording segment** that the chaos tests need moves to step 7.
  - **An engine that ends unexpectedly:** until step 7, the supervisor reports it and exits.
  - **Engine options** are passed through after `--`.
- **The API exists before the code:** the acceptance tests compile against the public headers. Until the maintainer approves this spec and the tests, the function bodies are stubs, and every acceptance test fails.

## Requirements

### Executables and libraries

1. **`kool_spin`**, the supervisor, lives in `src/apps/supervisor/`: a library, `ks::supervisor`, and a thin `main.cpp`. It depends only on `ks::ipc`, and through it `ks::core`, the standard library, and POSIX (ADR 0002 §1).
2. **`ks-engine`** lives in `src/apps/engine/`: a library, `ks::engine_app`, and a thin `main.cpp`. It depends on `ks::engine` and `ks::ipc`.
3. **Both are built into `bin/`** in the build directory, so `kool_spin` finds `ks-engine` next to itself (requirement 13).
4. **Rules from the earlier steps** (`specs/segments/spec.md` requirements 2 to 4) apply to both:
   - every descriptor they create is close-on-exec;
   - system calls interrupted by a signal are retried.

   Nothing in this spec runs on the audio thread, apart from the `ks::engine` code that already does.
5. **Lock names:** `kSupervisorLockName` is `"supervisor"`, and `kEngineLockName` is `"engine"`. Both are in `<ks/ipc/runtime_dir.h>`.

### Messages

6. **Five new types,** following `specs/socket-protocol/spec.md` requirements 20 and 21:

   | Type | Number | From, to | Body | Descriptors | Request ID |
   |---|---|---|---|---|---|
   | `Hello` | 4 | Client to supervisor | Name: text, 1 to 64 bytes | 0 | Non-zero |
   | `Welcome` | 5 | Supervisor to client | Empty | 1: the engine-state segment | The `Hello`'s |
   | `Stop` | 6 | Client to supervisor; supervisor to engine | Empty | 0 | Non-zero |
   | `Done` | 7 | The receiver of a `Stop` to its sender | Empty | 0 | The `Stop`'s |
   | `Ready` | 8 | Engine to supervisor | Empty | 0 | 0 |

   `kMaxClientNameSize` is 64. `decode()` refuses:
   - a `Hello` whose name is empty or longer than 64 bytes, with `BadField`;
   - a `Ready` with a non-zero request ID, with `WrongRequestId`.
7. **Answering,** in every process:
   - **`Ping`:** answered with a `Pong`.
   - **`Error`:** reported, never answered.
   - **A valid message the receiver doesn't take from that peer:** answered with `Error` `NotSupported`, carrying the message's request ID. The connection stays open.
   - **A frame the connection refuses:** answered with `Error` `BadMessage` and request ID 0, then the connection is closed.
   - **A frame that doesn't decode:** answered with `Error` `BadMessage` and the frame's request ID, then the connection is closed.

### The engine process

8. **Command line:** `ks-engine --supervisor-fd <n> --state-fd <n> --runtime-dir <path> [output options]`. The supervisor gives the first three. The output options come from after `kool_spin`'s `--`, and have the ranges and defaults of `ks-harness`'s (`specs/harness/spec.md` requirement 2):
   - `--backend <name>`: `system` (the default), `null`, `pulseaudio`, `alsa`, `jack`, or `coreaudio`;
   - `--device <n>`: a device number, as `ks-harness --list-devices` numbers them; the default is the default device;
   - `--rate <hz>`, from 8000 to 384000; `--channels <n>`, from 2 to 8; `--period <frames>`, from 1 to 4096; each defaults to the device's own;
   - `--help`: print the usage and exit with status 0.

   These are usage errors: a missing required option, an unknown or repeated option, a value out of range, and a device number that isn't listed. Each writes one line to standard error, starting `ks-engine: `, and exits with status 2.
9. **Starting,** in this order. If a step fails, the engine writes one line to standard error and exits with status 1:
   1. it checks that `--supervisor-fd` is an open stream socket and `--state-fd` is an open descriptor, and makes both close-on-exec;
   2. it takes the `engine` lock in the runtime directory;
   3. it maps the state segment read-write, as kind `EngineState` at `kEngineStateLayoutVersion`;
   4. it builds the engine on that segment, starting clean (step 7 adds resuming);
   5. it opens the device source with the output options. If no backend initializes, that's a failure;
   6. it starts an `OutputController`. A device that can't open isn't a failure: the controller freewheels;
   7. it sends `Ready`.
10. **Running:** the main thread is the engine's control thread. At least every 10 ms, it:
    - reads the supervisor's socket;
    - calls the output controller's `poll()`;
    - collects the engine's releases.

    From the supervisor, it takes `Ping` and `Stop`, and answers anything else as requirement 7 says.
11. **Stopping:** a `Stop`, or `SIGTERM`, `SIGINT`, or `SIGHUP`, stops the engine:
    1. it calls the output controller's `stop()`, which mutes and waits for the fade-out to reach silence, then closes the device;
    2. it replies `Done`, if a `Stop` asked;
    3. it exits with status 0.

    The last status block it publishes has `kStatusMuted` and `kStatusSilenced` set.
12. **Without a supervisor:** if the supervisor's socket closes, or the engine refuses a frame from it, the engine closes the socket and keeps playing (ADR 0003 §12). It plays until a signal in requirement 11 stops it.

### The supervisor

13. **Command line:** `kool_spin [options] [-- engine options]`.
    - `--runtime-dir <path>`: the default is `default_runtime_directory_path()`.
    - `--engine <path>`: the `ks-engine` executable. The default is `ks-engine` in the directory that holds `kool_spin`'s own executable.
    - `--help`: print the usage and exit with status 0.

    Everything after `--` goes to `ks-engine` unchanged, and the supervisor doesn't check it. A usage error writes one line to standard error, starting `kool_spin: `, and exits with status 2.
14. **Exit statuses:**
    - **0:** it stopped cleanly;
    - **1:** it couldn't start;
    - **2:** a usage error;
    - **3:** the engine ended unexpectedly, or didn't stop when asked.
15. **Starting,** in this order. If a step fails, the supervisor writes one line to standard error and exits with status 1. It leaves no socket file and no child process behind, and both locks are free:
    1. it creates or checks the runtime directory (`open_runtime_directory()`);
    2. it takes the `supervisor` lock. If another process holds it, the line gives that process's ID;
    3. it checks the `engine` lock. If it's held, an engine from an earlier session is still running, and the line gives its process ID. Step 8 decides what to do about that engine;
    4. it creates the engine-state segment and a `socket_pair()`;
    5. it starts `ks-engine` (requirement 17) with its end of the pair as descriptor 3 and the state segment as descriptor 4. The arguments are `--supervisor-fd 3 --state-fd 4 --runtime-dir <dir>`, then the engine options;
    6. it waits up to `kEngineReadyTimeout` (10 s) for `Ready`. If the engine exits first, that's a failure. So is no `Ready` in time: the supervisor kills the engine with `SIGKILL` and reaps it;
    7. it opens the supervisor socket. `supervisor.sock` therefore appears only once the engine is playing.
16. **The `poll` loop:** the supervisor has one thread. It polls the listener, the engine's connection, the clients' connections, and a self-pipe that the handlers for `SIGCHLD`, `SIGINT`, `SIGTERM`, and `SIGHUP` write to. Only `poll` blocks.
17. **Starting children:** `ks::supervisor::spawn()` starts a program with `posix_spawn`. It's public, so tests can use it. The child:
    - leads a new process group;
    - starts with an empty signal mask, and every signal at its default disposition;
    - inherits descriptors 0, 1, and 2, and each descriptor it's given, at the number it's given. It inherits no other descriptor, even one the supervisor itself inherited without close-on-exec;
    - doesn't ask to be killed when its parent dies.

    `spawn()` fails, with the error, if the program can't be started: for example, if it doesn't exist or isn't executable.
18. **Clients:**
    - **Accepting:** the supervisor accepts connections from its own effective user ID only.
    - **The connection limit:** at most `kMaxConnections` (16) connections at a time. One more is closed as soon as it's accepted.
    - **`Hello` first:** a client's first message must be `Hello`. Anything else is answered with `Error` `BadMessage`, and the connection is closed.
    - **`Welcome`:** the supervisor answers a `Hello` with a `Welcome` carrying the engine-state segment. The client maps it read-only and reads the engine's state. Step 3b adds commands.
    - **After `Hello`:** the supervisor takes `Ping` and `Stop`. A second `Hello` is answered with `Error` `NotSupported`.
    - **Dropping a client:** a client is dropped when it closes its connection, or when its outgoing queue is full. Nothing a client does affects the engine or the other clients.
19. **Stopping:** a client's `Stop`, or `SIGINT`, `SIGTERM`, or `SIGHUP`, stops the supervisor:
    1. it closes the listener, which removes the socket file. From then on, it answers `Hello` with `Error` `NotSupported`;
    2. it sends `Stop` to the engine, and waits up to `kEngineStopTimeout` (2 s) for the engine to exit;
    3. if the engine exits with status 0, the supervisor replies `Done` to every `Stop` it received. It sends what's queued for clients, for up to 1 s, closes every connection, and exits with status 0;
    4. if the engine exits with any other status, the supervisor reports it, closes every connection without replying `Done`, and exits with status 3. The same happens if the engine doesn't exit in time, after the supervisor kills it with `SIGKILL` and reaps it.

    While stopping, the supervisor still answers `Ping`, and takes more `Stop`s.
20. **The engine ending unexpectedly:** if the engine exits while the supervisor isn't stopping, the supervisor:
    1. reaps it;
    2. reports how it ended: its exit status, or the signal that killed it;
    3. closes the listener and every client's connection;
    4. exits with status 3.

    Step 7 replaces this with a restart.
21. **The engine's connection failing:** the engine might close its connection, or send a frame that's refused or doesn't decode. The supervisor then:
    1. sends the engine `SIGTERM`, which makes it fade out (requirement 11);
    2. waits up to `kEngineStopTimeout` for it to exit, and kills it with `SIGKILL` if it hasn't;
    3. exits with status 3.

    From the engine, the supervisor takes `Ready` (once, first), `Pong`, and `Done`. It answers anything else as requirement 7 says.
22. **Reports:** the supervisor writes one line to standard error for each of these events, each line starting `kool_spin: `:
    - the engine started, with its process ID;
    - the engine is ready;
    - a client said `Hello`, with its name;
    - a client was dropped, and why;
    - the supervisor is stopping;
    - how the engine ended.

    The wording isn't part of the interface: tests don't check it. Step 4 moves these lines to log files.

## Acceptance criteria

The tests are in `tests/acceptance/processes/`.

- **Separate runtime directories:** each test uses its own runtime directory, in a temporary directory, so tests can run in parallel.
- **The null backend:** tests that start `ks-engine` pass it `--backend null`.
- **Cleanup:** whatever happens, each test kills any process it started, so no process outlives the test.
- **The probe:** a helper program built for these tests, `ks_process_probe`. It writes a report of what it inherited: its open descriptors, process group, signal mask, signal dispositions, and arguments. It can also stand in for `ks-engine`, misbehaving on purpose.

Messages (`messages_test.cpp`):

- [ ] `[auto]` **Round trip:** each of these encodes and decodes to the same message, and encodes again to the same bytes:
  - `Hello` with names of 1 byte, 64 bytes, and multi-byte UTF-8;
  - `Welcome`, `Stop`, `Done`, and `Ready`.

  (test: "Process messages: round trip")
- [ ] `[auto]` **Refused:** each of these is refused with the right error:
  - `Hello` with an empty name, a 65-byte name, and invalid UTF-8;
  - `Welcome` with no descriptor, and with 2;
  - `Hello` and `Stop` with request ID 0, `Done` with request ID 0, and `Ready` with request ID 1;
  - a 1-byte body for each type whose body is empty.

  (test: "Process messages: what's refused")

Spawning (`spawn_test.cpp`):

- [ ] `[auto]` **Descriptors:** the test holds a descriptor without close-on-exec, and spawns the probe with two of its descriptors given as 3 and 5. The probe's open descriptors are exactly 0, 1, 2, 3, and 5, and 3 and 5 refer to the files the test gave (`fstat` device and inode). (test: "Spawning: only the given descriptors")
- [ ] `[auto]` **Process group and signals:** the test blocks `SIGUSR1` and ignores `SIGINT` and `SIGPIPE`, then spawns the probe. The probe:
  - leads its own process group, which isn't the test's;
  - has an empty signal mask;
  - has the default disposition for `SIGINT`, `SIGTERM`, `SIGHUP`, and `SIGPIPE`;
  - gets its arguments in order, including an empty one and one with spaces.

  (test: "Spawning: a new process group and default signals")
- [ ] `[auto]` **Failure:** a path that doesn't exist, and a file that isn't executable, are each refused with an error. No child process is left. (test: "Spawning: a program that can't start")

The engine process (`engine_process_test.cpp`). In these tests, the test plays the supervisor. It creates the runtime directory, the state segment, and a `socket_pair()`, and spawns `ks-engine` with `--backend null --rate 44100 --channels 2`:

- [ ] `[auto]` **Ready:** `Ready` arrives within 5 s, with request ID 0. Then:
  - the status block's heartbeat keeps increasing, its output state is playing, and its sample rate is 44100;
  - the `engine` lock is held, with the engine's process ID;
  - a `Ping` is answered with a `Pong`.

  (test: "Engine process: starts and reports Ready")
- [ ] `[auto]` **`Stop`:** a `Stop` is answered with `Done`, with the `Stop`'s request ID. The last status block has `kStatusMuted` and `kStatusSilenced` set. The engine exits with status 0, and the `engine` lock is free. (test: "Engine process: Stop fades out and exits")
- [ ] `[auto]` **Signals:** `SIGTERM`, `SIGINT`, and `SIGHUP`, each sent to its own engine, end it the same way: muted and silenced, then exit status 0. (test: "Engine process: signals fade out and exit")
- [ ] `[auto]` **Without a supervisor:** the test closes its end of the socket. The heartbeat keeps increasing for 500 ms. Then `SIGTERM` ends the engine, muted and silenced, with exit status 0. (test: "Engine process: keeps playing without its supervisor")
- [ ] `[auto]` **What it refuses:**
  - a `Hello` is answered with `Error` `NotSupported`, and a `Ping` sent after it is still answered;
  - a frame with a bad header is answered with `Error` `BadMessage`, and then the socket closes. The heartbeat keeps increasing.

  (test: "Engine process: what it refuses from its supervisor")
- [ ] `[auto]` **Start failures:**
  - **exit status 2:** no `--state-fd`; an unknown option; `--rate 5`; `--device 1` on the null backend, which lists one device;
  - **exit status 1:** the `engine` lock already held by the test; a state segment of the wrong kind (a command-ring segment); a `--supervisor-fd` that's a regular file; a runtime directory that doesn't exist.

  (test: "Engine process: start failures")

The supervisor (`supervisor_test.cpp`). In these tests, the test spawns `kool_spin --runtime-dir <dir> --engine <ks-engine> -- --backend null`:

- [ ] `[auto]` **Starting:** `supervisor.sock` appears within 5 s. Then:
  - a `Hello` is answered with a `Welcome` whose descriptor maps as the engine-state segment, and its heartbeat keeps increasing;
  - the `supervisor` lock is held with `kool_spin`'s process ID;
  - the `engine` lock is held with another process ID, and that process leads its own process group;
  - a `Ping` is answered with a `Pong`.

  (test: "Supervisor: starts the engine and welcomes clients")
- [ ] `[auto]` **A client's `Stop`:** it's answered with `Done`. Then:
  - the status block has `kStatusMuted` and `kStatusSilenced` set;
  - `kool_spin` exits with status 0;
  - the socket file is gone, and both locks are free.

  (test: "Supervisor: a client's Stop stops everything")
- [ ] `[auto]` **Signals:** each of these does the same as a client's `Stop`, without the `Done`: `SIGINT`, `SIGTERM`, and `SIGHUP` sent to `kool_spin`, and `SIGINT` sent to its process group, as Ctrl-C at a terminal does. (test: "Supervisor: signals stop everything")
- [ ] `[auto]` **The engine killed:** after `SIGKILL` to the engine:
  - `kool_spin` exits with status 3 within 5 s;
  - the client's connection closes;
  - the socket file is gone, and the `supervisor` lock is free.

  (test: "Supervisor: exits when the engine ends")
- [ ] `[auto]` **The supervisor killed:** after `SIGKILL` to `kool_spin`:
  - the client's connection closes;
  - the engine's heartbeat keeps increasing for 500 ms, and the `engine` lock stays held;
  - `SIGTERM` to the engine then ends it, muted and silenced, and frees the `engine` lock.

  (test: "Supervisor: the engine outlives it")
- [ ] `[auto]` **One at a time:**
  - while one `kool_spin` runs, a second with the same runtime directory exits with status 1, and the first still answers a `Ping`;
  - while the test holds the `engine` lock, `kool_spin` exits with status 1, without starting an engine or leaving a socket file.

  (test: "Supervisor: one at a time")
- [ ] `[auto]` **An engine that doesn't start:** in each case, `kool_spin` exits with status 1, leaves no socket file, and both locks are free:
  - engine options with `--rate 5`;
  - an `--engine` path that doesn't exist;
  - the probe as the engine, never sending `Ready`. The exit comes after 10 s, and the probe has been killed.

  (test: "Supervisor: an engine that doesn't start")
- [ ] `[auto]` **An engine that won't stop:** the probe as the engine sends `Ready`, then ignores `Stop` and `SIGTERM`. After a client's `Stop`, `kool_spin` kills the probe and exits with status 3, within 2 s plus a margin. (test: "Supervisor: an engine that won't stop is killed")
- [ ] `[auto]` **What it refuses from clients:**
  - a client whose first message is a `Ping` gets `Error` `BadMessage`, and its connection closes;
  - a client that sends a frame with a bad header has its connection closed;
  - a second `Hello` is answered with `Error` `NotSupported`, and so is a `Pong`;
  - a client that disconnects doesn't affect another client, which still gets a `Pong`;
  - with 16 connections open, a 17th is closed at once, and the 16 still answer.

  (test: "Supervisor: what it refuses from clients")
- [ ] `[auto]` **Usage:** an unknown option exits with status 2, and `--help` with status 0. (test: "Supervisor: usage")

Everywhere:

- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux in every preset, and on macOS arm64 in `debug`, `release`, `asan`, and `tsan`.
- [ ] `[auto]` **Fuzzing:** the socket-protocol fuzz target, `ks_socket_protocol_fuzz`, decodes the new types without further changes, and still runs 1,000,000 inputs clean under the `fuzz` preset.

## Out of scope

- **Step 3b:** command rings for clients, and attaching them to the engine and detaching them.
- **Step 4:** logs. Reports go to standard error until then.
- **Step 5:** tracks and the decode worker.
- **Step 6:** `ks-play`.
- **Step 7:**
  - restarting the engine;
  - watching the heartbeat, and pinging the engine;
  - the crash-loop guard;
  - the recording segment for chaos tests.
- **Step 8:** what a relaunched supervisor does about a running engine, beyond refusing to start.
- **Step 10:** health checks, memory locking, real-time priority, and blocking sleep.
- **A timeout for `Hello`:** a client that never sends it holds one of the 16 connections until it disconnects.
- **Installing,** and Windows.

## Open questions

None. The maintainer approved these decisions:

1. **Message types** (requirement 6):
   - one `Stop` type, sent by a client to the supervisor and by the supervisor to the engine. In both cases it means "fade out, stop, and reply `Done`";
   - `Done` is a general, empty reply;
   - `Ready` is a notification, with request ID 0;
   - `Welcome` carries only the state segment. Step 3b gives a client its ring through a request of its own, so clients that only watch, such as a monitor or a test, don't take one of the engine's 8 client slots.
2. **The engine's descriptors are inherited** (requirement 15): the socket at 3 and the state segment at 4, named on the command line, rather than sent in a first message. After a crash, step 7 starts the new engine the same way.
3. **`supervisor.sock` appears only once the engine is ready** (requirement 15). A client that connects therefore always finds a playing engine, and tests can wait for the file.
4. **Clients** (requirement 18): `Hello` first, at most 16 connections, and no `Hello` timeout.
5. **Exit statuses and timeouts** (requirements 14, 15, and 19): 0, 1, 2, and 3; 10 s for `Ready` (opening a device can be slow); 2 s for the engine to stop (the fade takes 10 ms).
6. **An engine lock held at launch** (requirement 15): the supervisor refuses to start, and gives the engine's process ID. Step 8 adds the report and the offer to stop it.
7. **The engine without a supervisor** (requirements 11 and 12): it keeps playing until `SIGTERM`, `SIGINT`, or `SIGHUP`, any of which fades it out. A relaunched supervisor in step 8 can stop it the same way.
8. **`bin/` in the build directory** (requirement 3), so the default `--engine` works in the build tree as well as in an installation.
9. **The engine's options mirror `ks-harness`'s device options** (requirement 8), including device numbers from `ks-harness --list-devices`. `ks-engine` has no `--list-devices` of its own.
