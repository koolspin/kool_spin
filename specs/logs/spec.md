# Feature: Per-process logs

- **Status:** Approved by the maintainer on 2026-11-15; what reaches standard error ("This spec extends" and decision 3) amended on 2026-11-16, after the build loop's review. Protected: build loops can't change it.
- **Roadmap:** Phase 1b, step 4a
- **Built by:** loop

## Goal

Give every process a log file, with timestamps that line up across processes. Each child writes its lines into a ring in a shared-memory segment that the supervisor created. The supervisor writes those lines, and its own, into one file per process, in a folder for the session. A child that crashes leaves its last lines behind, because the supervisor still holds its ring.

## Context

- **ADRs that apply:**
  - **ADR 0003 §13:** each process logs through a ring in a shared-memory segment that the supervisor created, and the supervisor writes the rings to per-process log files. Non-real-time threads write to the ring under a mutex. When a ring is full, new lines are dropped and counted. Timestamps come from the monotonic clock, which all processes share, and each log file records the wall-clock time it started. The audio thread's own events are step 4b (see "Out of scope").
  - **ADR 0003 §7:** a log segment is written by its process. The engine doesn't map any other process's log.
  - **ADR 0003 §8:** shared memory is copied, validated, and only then used. That applies to the supervisor reading a child's log: nothing a child writes into its ring can crash the supervisor, or put anything but well-formed lines into a file.
  - **ADR 0002:** the supervisor depends on the standard library only.
- **What exists:**
  - **`ks::ipc`:** `SpscRing`, `RingProducer`, and `RingConsumer` (a handle that finds the counters inconsistent marks the ring corrupt and stops using it); `SegmentKind::Log`; `SegmentFactory`, `Region`, and the runtime directory.
  - **`kool_spin` and `ks-engine`** (`specs/processes/spec.md`, `specs/client-rings/spec.md`): both write their reports to standard error, one line each, starting `kool_spin: ` or `ks-engine: `.
  - **`ks::engine::OutputController`:** `state()` gives the output state: stopped, playing, freewheeling, or attaching. It runs on the control thread.
- **This spec extends** `specs/processes/spec.md`: requirement 8 (the engine's command line), 9 (the engine starting), 13 (the supervisor's command line), 15 (the supervisor starting), 16 (the `poll` loop), and 22 (reports); and `specs/client-rings/spec.md` requirement 13 (reports). Reports become log lines (requirements 15 and 18 here), so less reaches standard error: with a log file, `kool_spin`'s `info` lines go only to `kool_spin.log`, and with `--log-fd`, the engine's reports go only to its log. Everything else in those specs still applies unchanged, including usage errors and the starting failures that come before a log is open.
- **The API exists before the code:** the acceptance tests compile against the public headers. Until the maintainer approves this spec and the tests, the function bodies are stubs, and every acceptance test fails.

## Requirements

### The log segment

1. **Layout,** in `<ks/ipc/log.h>`:
   - `kLogLayoutVersion` is 1;
   - `LogLevel`: `Error` = 1, `Warning` = 2, `Info` = 3;
   - `LogRecord`, 256 bytes, every field a `std::uint64_t` except the text:
     - `time`: when the line was written, in nanoseconds of `std::chrono::steady_clock`, which every process on the machine shares;
     - `level`: a `LogLevel` value;
     - `length`: the text's length in bytes, at most `kLogTextSize` (224);
     - `flags`: `kLogTruncated` (bit 0) if the line was cut short, and no other bits;
     - `text`: `kLogTextSize` bytes, of which the first `length` are the line, in UTF-8, with no newline;
   - `LogRing` is `SpscRing<LogRecord, kLogRingCapacity>`, and `kLogRingCapacity` is 1,024;
   - `LogSegment` holds a `LogRing`, then `dropped`: how many lines the writer has dropped because the ring was full. It's written by the writer only, at least 128 bytes from the ring's tail;
   - `kLogSegmentSize` is `kSegmentHeaderSize + sizeof(LogSegment)`, and a log segment holds a `LogSegment` at offset `kSegmentHeaderSize`.

   The supervisor creates each log segment with `SegmentFactory::create(SegmentKind::Log, kLogLayoutVersion, kLogSegmentSize)`. A new segment is zero-filled apart from its header, so its ring is empty and `dropped` is 0.
2. **Rules from the earlier steps** (`specs/segments/spec.md` requirements 2 to 4) apply: every descriptor is close-on-exec, and system calls interrupted by a signal are retried. Nothing in this spec runs on the audio thread.

### Writing and reading

3. **`LogWriter`** writes into a `LogSegment` that outlives it:
   - `write(level, text)` stamps the line with `steady_clock` and pushes it as one record. It may be called from any thread except the audio thread. Calls are serialized by a mutex, so each thread's lines keep their order;
   - a line longer than `kLogTextSize` bytes is cut at the last UTF-8 character boundary that fits, and gets `kLogTruncated`. A newline in the text, and anything after it, is cut the same way;
   - when the ring is full, the line is dropped, and `dropped` goes up by one. `write()` never waits for the reader;
   - the writer keeps its counts in private memory, as `RingProducer` does, and only ever writes them to the segment.
4. **`LogReader`** reads a `LogSegment` that outlives it:
   - `next()` returns a copy of the oldest record, or nothing if the ring is empty or corrupt;
   - `take_dropped()` returns how many lines were dropped since its last call, from `dropped`. A `dropped` lower than the last value it read makes the reader corrupt;
   - `corrupt()` is true once the ring's counters, or `dropped`, have been found inconsistent. From then on the reader never touches the segment again.

   The reader doesn't trust a record's contents: formatting it (requirement 6) validates them.

### Log lines

5. **The clock:** `LogClock` pairs the steady clock with the wall clock at one moment: a steady time, a system time, and the local UTC offset at that moment, in seconds. `LogClock::now()` reads all three. The supervisor reads it once, as the session starts, and uses that one `LogClock` for every line of every file in the session. A line's time is therefore its steady time, moved by one fixed offset: lines from different files sort together, and a wall-clock change during the session can't reorder them.
6. **Format,** from `format_log_line(clock, time, level, text)` and `format_log_record(clock, record)`, with no newline:
   - **A line** is `YYYY-MM-DD HH:MM:SS.ffffff <level> <text>`. The time is the steady time converted with the clock (requirement 5), shown in the session's local time to the microsecond, rounded down. `<level>` is `error`, `warning`, or `info`;
   - **a time that can't be shown,** because it falls before the year 1970 or after 9999, is written as `steady:<n>` instead, where `<n>` is the steady time in nanoseconds. A `LogClock`'s steady time is a `std::uint64_t` and its system time a `std::int64_t`, both in nanoseconds, and the conversion never overflows;
   - **the text** is copied as it is, except: each byte of a control character (U+0000 to U+001F, U+007F, and U+0080 to U+009F), and each byte that isn't part of valid UTF-8, is written as `\xNN`, with two lowercase hexadecimal digits. Every formatted line is therefore valid UTF-8 with no control characters;
   - **a truncated record** has `…` (U+2026) after its text;
   - **an invalid record,** whose level isn't 1, 2, or 3, whose length is more than `kLogTextSize`, or whose flags have a bit other than `kLogTruncated`, becomes `<time> warning [invalid log record]`, with the record's time.
7. **The header:** `format_log_header(process, clock)` is the first line of every file:

   `# <process> log; session started YYYY-MM-DD HH:MM:SS.ffffff ±HH:MM; steady clock <s>.<nnnnnnnnn>`

   with the clock's system time in local time, its UTC offset, and its steady time in seconds to the nanosecond. Every file in a session has the same values.

### Log files

8. **`ks::supervisor::LogFile`** writes one log file:
   - in `<ks/supervisor/log_file.h>`, `create(path, process, clock, limit)` creates the file, failing if it exists, with mode 0600 and close-on-exec, and writes the header. `limit` defaults to `kLogFileLimit`, 16 MiB;
   - `write(time, level, text)` and `write(record)` each format one line, and write it with its newline in a single `write()` call, unbuffered, so lines written before a crash are in the file;
   - **the limit:** when a line would take the file past `limit` bytes, the file gets one last line instead, a warning that it has reached its limit and later lines are dropped. Every later line is dropped;
   - **a write error:** the file stops being written, and `error()` returns the error. Each `write()` returns whether the line was written.
9. **The folder:** `kool_spin --log-dir <path>` names the folder that holds the sessions. The default is:
   - **Linux:** `$XDG_STATE_HOME/kool_spin/logs` if `XDG_STATE_HOME` is set to an absolute path, and `$HOME/.local/state/kool_spin/logs` otherwise;
   - **macOS:** `$HOME/Library/Logs/kool_spin`.

   The supervisor creates the folder and any missing parents, with mode 0700. With no `--log-dir` and no usable `HOME`, there's no folder (requirement 13).
10. **Sessions:** each launch writes its files in a new folder inside the log folder, named for the session's start in local time and the supervisor's process ID: `YYYY-MM-DD_HH-MM-SS_<pid>`. It holds `kool_spin.log`, and `ks-engine.log` once the engine has been started.
11. **Keeping ten:** before creating its session folder, the supervisor keeps the 9 newest folders whose names have the session pattern, ordered by the time in their names, then by process ID, and removes the others. Removing one means deleting the regular files in it whose names end in `.log`, without following symbolic links, then the folder itself if it's empty. Nothing else in the log folder, or in a session folder, is ever deleted.

### The supervisor

12. **Starting:** the supervisor opens its log after it has taken the `supervisor` lock (`specs/processes/spec.md` requirement 15, step 2), and before checking the `engine` lock:
    1. it reads the `LogClock`;
    2. it creates the log folder, removes old sessions (requirement 11), creates the session folder with mode 0700, and creates `kool_spin.log`;
    3. it creates a log segment for the engine with the state segment, and passes it to `ks-engine` as descriptor 5. If the segment can't be created, that's a starting failure, as for the state segment. The engine's arguments become `--supervisor-fd 3 --state-fd 4 --log-fd 5 --runtime-dir <dir>`, then the engine options. It creates `ks-engine.log` just before starting the engine.

    `--help`, a usage error, and a failure before the log is open (the runtime directory, or the `supervisor` lock) create no session folder.
13. **Without a log folder:** if the log folder, the session folder, or `kool_spin.log` can't be created, the supervisor writes one warning to standard error saying why, and runs without log files. Every line it would have written to a log file goes to standard error instead, starting `kool_spin: ` for its own lines and `ks-engine: ` for the engine's. If `ks-engine.log` alone can't be created, the engine's lines go to standard error the same way. A log file that fails later (requirement 8) is reported once, in `kool_spin.log` if that still works, and on standard error. Logging never stops the supervisor or the engine.
14. **Reading the engine's log:** the supervisor maps the engine's log segment read-write, and reads its ring with a `LogReader`:
    - at least every `kLogDrainInterval` (100 ms, in `<ks/supervisor/run.h>`) while the engine runs: the `poll` loop's timeout is never longer;
    - once more after the engine has exited and been reaped, before the supervisor reports how it ended, so the engine's last lines are in its file even after `SIGKILL`.

    Each record goes into `ks-engine.log` with `format_log_record()`. When `take_dropped()` returns more than 0, `ks-engine.log` gets a warning saying how many lines were dropped because its ring was full. When the reader finds the ring corrupt, `ks-engine.log` and `kool_spin.log` each get an error saying so, and the ring isn't read again. The supervisor keeps its descriptor and mapping of the segment until it exits.
15. **The supervisor's own lines** go to `kool_spin.log`, with these levels. Its warnings and errors also go to standard error, as one line starting `kool_spin: ` (decision 3):

    | Level | Events |
    |---|---|
    | `info` | The engine started, with its process ID; the engine is ready; a client said `Hello`, with its name; a client asked for a ring; a client's ring opened, with its slot; a ring was detached, with its slot; a client closed its connection; the supervisor is stopping, and why; the engine stopped cleanly. |
    | `warning` | A client was dropped for any other reason, and why; a client's `OpenRing` was refused, and why; a connection was refused, and why; a client or the engine sent an `Error`, with its text; the engine refused to detach a ring. |
    | `error` | A failure while starting; the engine ended unexpectedly, and how; the engine didn't send `Ready`, or didn't stop, in time, and was killed; the engine's connection failed; `poll` failed; a log file failed; the engine's log ring was found corrupt. |

### The engine

16. **Command line:** `ks-engine` also takes `--log-fd <n>`: a log segment. It's optional. A repeated `--log-fd` is a usage error.
17. **Starting:** the engine's first starting step (`specs/processes/spec.md` requirement 9) also checks that `--log-fd`, if given, is an open descriptor, and makes it close-on-exec. Then, before taking the `engine` lock, the engine maps the segment read-write as kind `Log` at `kLogLayoutVersion`, finds the `LogSegment` at `kSegmentHeaderSize` using its own mapping's size, and starts a `LogWriter` on it. If that fails, it's a starting failure: one line to standard error, and exit status 1.
18. **Its lines:** with `--log-fd`, every report goes to the log, and nothing goes to standard error except starting failures, which go to both. Without `--log-fd`, reports go to standard error, as before. The levels:

    | Level | Events |
    |---|---|
    | `info` | It's ready; it attached a ring, with the slot; it detached a ring, with the slot; the output started playing on a device, with the device's sample rate and channel count; it's stopping, and why; it has stopped. |
    | `warning` | The output is freewheeling: no device opened, or the device was lost; it refused an `Attach` or a `Detach`, and why; the supervisor sent an `Error`, with its text; the supervisor's connection closed, and it plays on. |
    | `error` | A failure while starting. |

    The output lines come from the control thread: after each `OutputController::poll()`, and after `start()`, the engine compares `state()` with the last state it logged, and logs a change to playing or freewheeling. The device's format may come from a new control-thread accessor on `OutputController`; nothing on the audio thread changes.

### The lines tests look for

19. **Fixed phrases:** the wording of log lines isn't part of the interface, except that these lines contain these phrases, where `<n>` is a slot number and `<name>` a client's name:

    | File | Level | Contains |
    |---|---|---|
    | `ks-engine.log`, or the engine's log segment | `info` | `ready` |
    | Same | `info` | `attached a ring in slot <n>` |
    | Same | `info` | `stopped` |
    | Same | `info` | `output playing` |
    | Same | `warning` | `output freewheeling` |
    | `kool_spin.log` | `info` | `client '<name>' asked for a ring` |
    | Same | `info` | `client '<name>' opened a ring in slot <n>` |
    | Same | `warning` | `refused client '<name>' a second ring` |
    | Same | `error` | `ks-engine was killed by signal <n>` |

## Acceptance criteria

The tests are in `tests/acceptance/logs/`. They follow `specs/processes/spec.md`'s rules for tests: separate runtime directories, the null backend, and no process outliving its test. Every test that starts `kool_spin` passes `--log-dir` inside its own temporary directory, except the default-folder test, which points `HOME` and `XDG_STATE_HOME` there. Where a test reads standard error, it starts `kool_spin` through `/bin/sh`, which redirects it to a file and then runs `kool_spin` with `exec`.

"A well-formed file" below means: its first line is a header (requirement 7) naming its process, and every other line matches `^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6} (error|warning|info) .*$` and is valid UTF-8 with no control characters.

The segment, the writer, the reader, and the formats (`log_test.cpp`):

- [ ] `[auto]` **Layout:** `sizeof(LogRecord)` is 256, `kLogTextSize` is 224, `kLogRingCapacity` is 1,024, `kLogLayoutVersion` is 1, and `kLogSegmentSize` is `kSegmentHeaderSize + sizeof(LogSegment)`. A segment created as requirement 1 says maps read-write as a log segment, its ring is empty, and `dropped` is 0. (test: "Log segments: layout")
- [ ] `[auto]` **In order, from many threads:** four threads each write 200 lines with their own text and a level each. A reader, reading while they write, gets all 800, each thread's in the order it wrote them, with their levels and texts. Each thread's times never decrease, and every time lies between the test's steady-clock readings before and after. Nothing is dropped. (test: "Log writer: lines in order, from many threads")
- [ ] `[auto]` **A full ring:** with no reader, 1,034 lines are written. `take_dropped()` returns 10, then 0. The reader gets the first 1,024 lines, in order. A line written after that is read too. (test: "Log writer: a full ring drops lines and counts them")
- [ ] `[auto]` **Truncating:** a 300-byte ASCII line arrives as its first 224 bytes, with `kLogTruncated`. A line of 150 two-byte characters arrives as 112 whole characters, and one of 100 three-byte characters as 74, both with `kLogTruncated`. A 224-byte line arrives whole, without it. `"one\ntwo"` arrives as `one`, with `kLogTruncated`. (test: "Log writer: long lines are truncated")
- [ ] `[auto]` **A corrupt ring:** with the head set 2,000 past the tail, `next()` returns nothing and `corrupt()` is true. In a second segment, `dropped` set to 5, then to 3, makes `take_dropped()` return 5, then the reader corrupt. (test: "Log reader: a corrupt ring")
- [ ] `[auto]` **Formats,** with a fixed `LogClock` of steady time 1,000 s, system time 2026-11-15 23:22:03 UTC, and a UTC offset of −5 hours:
  - a line at steady time 1,000.5 s, `info`, `hello` is `2026-11-15 18:22:03.500000 info hello`; at 1,000.000000999 s it shows `.000000`;
  - `error` and `warning` lines show their level;
  - in the text, the bytes `61 09 62` come out as `a\x09b`; `7f` as `\x7f`; `c2 85` (U+0085) as `\xc2\x85`; a lone `ff` as `\xff`; and `e2 82`, a character cut short, as `\xe2\x82`. The bytes `c3 a9` (`é`) and `e2 80 a6` (`…`) come out as themselves;
  - a record with `kLogTruncated` has `…` after its text;
  - records with level 0, level 4, length 225, and flags 2 each become `<time> warning [invalid log record]`;
  - with a second clock, whose steady time is 2⁶³ ns, a time of 0 falls before 1970 and comes out as `steady:0`;
  - the header for `ks-engine` is `# ks-engine log; session started 2026-11-15 18:22:03.000000 -05:00; steady clock 1000.000000000`.

  (test: "Log lines: format")
- [ ] `[auto]` **The size limit:** a `LogFile` with a 4 KiB limit takes lines until one would pass the limit. That line isn't written; the file's last line is the limit warning, and it ends at most one line past 4 KiB. Every later `write()` returns false and leaves the file unchanged. The file has mode 0600, and `create()` fails on a path that exists. (test: "Log files: the size limit")

The engine (`engine_logs_test.cpp`). In these tests, the test plays the supervisor, as in `specs/processes/spec.md`'s engine tests, and creates the log segment as requirement 1 says:

- [ ] `[auto]` **Through its segment:** `ks-engine` started with `--log-fd 5`, with standard error redirected to a file. From the segment, the test reads `info` lines containing `ready` and `output playing`, and, after an `Attach`, `attached a ring in slot 0`. Every line's time lies between the test's steady-clock readings before the start and after reading it. After `SIGTERM`, the engine exits with status 0, the last line contains `stopped`, and the standard-error file is empty. (test: "Engine logs: through its segment")
- [ ] `[auto]` **A bad log descriptor:** `--log-fd` naming a closed descriptor, and `--log-fd` naming an engine-state segment, each make the engine exit with status 1, without taking the `engine` lock. `--log-fd` given twice makes it exit with status 2. (test: "Engine logs: a bad log descriptor")
- [ ] `[auto]` **A full ring never blocks it:** with no one reading its log, the test sends 1,100 `Detach`es for slot 5, which holds no ring; each refusal is a warning line. Every `Detach` is answered with `Error` `Unavailable`, the heartbeat keeps increasing, and afterwards `dropped` is more than 0 and the ring holds 1,024 records. A `Ping` is still answered. (test: "Engine logs: a full ring never blocks it")

The supervisor (`supervisor_logs_test.cpp`). In these tests, the test spawns `kool_spin` with `ks-engine`, as in `specs/processes/spec.md`'s supervisor tests:

- [ ] `[auto]` **A session folder:** after `kool_spin` starts and a client says `Hello`, the log folder holds one folder whose name matches `^\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}_<pid>$`, with `kool_spin`'s process ID, and mode 0700. It holds `kool_spin.log` and `ks-engine.log`, each with mode 0600. After a client's `Stop`, `kool_spin` exits with status 0, both files are well-formed, both headers have the same session start and steady clock, `ks-engine.log` has `info` lines containing `ready` and `stopped`, and the header's session start is within 5 s of the test's wall clock at launch. (test: "Logs: a session folder")
- [ ] `[auto]` **Shared timestamps:** a client named `timer` opens a ring. Within 2 s, `kool_spin.log` has `client 'timer' asked for a ring` at time A and `client 'timer' opened a ring in slot 0` at time C, and `ks-engine.log` has `attached a ring in slot 0` at time B, with A ≤ B ≤ C. (test: "Logs: shared timestamps")
- [ ] `[auto]` **The engine's last lines outlive it:** a client opens a ring. As soon as `RingOpened` arrives, the test kills the engine with `SIGKILL`. `kool_spin` exits with status 3. `ks-engine.log` is well-formed and contains `attached a ring in slot 0`, and `kool_spin.log` has an `error` line containing `ks-engine was killed by signal 9`. (test: "Logs: the engine's last lines outlive it")
- [ ] `[auto]` **Keeping ten:** before launch, the log folder holds 12 session folders dated 2020-01-01 to 2020-01-12, each with a `kool_spin.log`; a folder named `notes` with a file in it; a file named `2020-01-01_00-00-00_1.log`; and the 2020-01-02 folder also holds `readme.txt`. After launch, the session folders from 2020-01-04 to 2020-01-12 and the new one remain. 2020-01-01 and 2020-01-03 are gone. 2020-01-02 remains with only `readme.txt`. `notes`, its file, and the `.log` file are untouched. (test: "Logs: keeps the newest ten sessions")
- [ ] `[auto]` **The default folder:** with `HOME` and `XDG_STATE_HOME` pointing at the test's directory, and no `--log-dir`, the session folder appears in `$XDG_STATE_HOME/kool_spin/logs` on Linux and `$HOME/Library/Logs/kool_spin` on macOS. On Linux, with `XDG_STATE_HOME` unset, it appears in `$HOME/.local/state/kool_spin/logs`. (test: "Logs: the default folder")
- [ ] `[auto]` **Without a log folder:** with `--log-dir` naming a regular file, `kool_spin` still starts, and a client is welcomed and can open a ring. Its standard error has a line starting `kool_spin: ` that contains the `--log-dir` path, a line starting `ks-engine: ` containing `ready`, and a line starting `kool_spin: ` containing `client 'nofiles' asked for a ring`. (test: "Logs: without a log folder")
- [ ] `[auto]` **Standard error:** a client named `greedy` says `Hello`, opens a ring, and asks for a second. `kool_spin`'s standard error has a line starting `kool_spin: ` containing `refused client 'greedy' a second ring`, and no line containing `asked for a ring`, and none starting `ks-engine: `. (test: "Logs: standard error gets kool_spin's warnings and errors")

Everywhere:

- [ ] `[auto]` **Fuzzing:** `ks_log_record_fuzz` formats arbitrary bytes as a `LogRecord`, with an arbitrary `LogClock`. Every result is one valid UTF-8 line with no control characters, and it runs 1,000,000 inputs clean under the `fuzz` preset. (test: `tests/acceptance/logs/record_fuzz.cpp`)
- [ ] `[auto]` **The earlier steps' tests** all still pass. Three protected test helpers change, so that they never write to the real log folder (decision 8): `KoolSpin` in `tests/acceptance/client-rings/rings_support.h`, and `Supervisor::start()` and `kool_spin_args()` in `tests/acceptance/processes/supervisor_test.cpp`, each pass `--log-dir` inside the test's runtime directory.
- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux in every preset, and on macOS arm64 in `debug`, `release`, `asan`, and `tsan`.
- [ ] `[manual]` **Device events:** with `kool_spin` playing through the Inpulse 500, pulling its USB cable and plugging it back in adds a `warning` line about freewheeling and then an `info` line about playing, with the device's rate and channels, to `ks-engine.log`, at the times it happened. Checked on Linux and on macOS, in the default log folder.

## Out of scope

- **Step 4b,** built interactively: the audio thread's events (late blocks, rejected commands, corrupt client rings, non-finite samples, decks waiting for decoding), recorded in a private lock-free ring and formatted by the control thread, with summaries so an overloaded engine can't flood its log.
- **Step 5:** the decode worker's log segment and file. It uses this mechanism unchanged.
- **Step 7:** a crash handler that records a backtrace (ADR 0002), and a restarted engine's lines, which go on in the same file.
- **Step 8:** what a supervisor that finds a running engine logs. Until then, a running engine's log ring fills up once its supervisor is gone, and new lines are dropped and counted.
- **Logs for clients** such as `ks-play` and the UI: they don't get log segments yet.
- **`ks-harness` and `ks-tone`:** they keep writing to standard error.
- **A debug level, log viewers, and tools that merge files.**

## Open questions

None. The maintainer approved these decisions; the first four are the maintainer's answers of 2026-11-15:

1. **Step 4 is split** (Goal; "Out of scope"). This step, 4a, is built by a loop and adds nothing to the audio thread. Step 4b, the audio thread's events, is built interactively, as `docs/process.md` asks for real-time code.
2. **One folder per session, ten kept, 16 MiB per file** (requirements 8 to 11). A bug report is one folder. Removing old sessions deletes only `.log` files in folders named like sessions.
3. **Standard error gets `kool_spin`'s warnings and errors** (requirement 15), as well as usage errors and failures before the log is open. Everything else goes only to the files: `kool_spin`'s `info` lines, which the earlier specs sent to standard error, and every line of the engine's except its starting failures (requirement 18).
4. **Lines show local date and time,** converted from the shared steady clock with one offset for the whole session (requirements 5 to 7).
5. **The log segment goes to the engine at launch,** as descriptor 5 (requirement 12), so the engine can log from its first starting step. The socket-protocol spec expected a message for it; none is needed.
6. **Fixed-size records:** 256 bytes, with 224 bytes of text, 1,024 to a ring: 256 KiB per process (requirement 1). Longer lines are cut at a character boundary.
7. **Three levels:** `error`, `warning`, and `info` (requirement 1). No debug level yet.
8. **A protected test change:** the earlier steps' tests start `kool_spin` without `--log-dir`, so they would write sessions into the real log folder, and remove real sessions. Three helpers add `--log-dir` inside each test's runtime directory ("Everywhere").
9. **No log folder isn't fatal** (requirement 13): `kool_spin` runs, and its lines and the engine's go to standard error instead.
10. **`--log-fd` is optional** (requirement 16): without it, `ks-engine` writes to standard error as before, so the processes tests that play the supervisor don't change.
