# Feature: Shared-memory segments

- **Status:** Approved by the maintainer on 2026-11-15. Protected: build loops can't change it.
- **Roadmap:** Phase 1b, step 1
- **Built by:** interactive

## Goal

Give kool_spin's processes real shared memory. The supervisor creates segments that have no names, the processes pass them to each other as file descriptors over Unix sockets, and each process maps them after checking them. The supporting pieces come with it: the per-user runtime directory, and the lock files that show which processes are alive. The rings and seqlocks from Phase 1a work in these segments unchanged.

## Context

- **ADR 0003 sections that apply:**
  - §6: the supervisor creates every segment and passes it on with `SCM_RIGHTS`; the runtime directory for client sockets.
  - §7: segments are created with no names, sealed on Linux, unlinked straight away on macOS, with the macOS intent file; every segment starts with a header.
  - §8: a segment's size is the mapping process's own, from `fstat`, never from a header.
  - §9: track segments are sized at the 2 GiB cap, and "Phase 1b verifies this lazy commitment on macOS".
  - §12: lock files show which processes are alive.
- **What exists** (`ks::ipc`, from Phase 1a):
  - `Region::create_anonymous()`: shared memory for processes created with `fork()`.
  - `SegmentHeader`, `SegmentKind`, `write_header()`, and `check_header()`.
  - The rings and seqlocks, which work in any region.
- **Platform differences:**
  - macOS has no `memfd_create`.
  - It limits POSIX shared-memory names to 31 characters.
  - It sizes a shared-memory object only once.
  - It can't list shared-memory names, so a name left behind by a crash stays until reboot unless something remembers it.
  - Its `recvmsg` has no `MSG_CMSG_CLOEXEC`.
  - Its `send` has no `MSG_NOSIGNAL`; the socket option `SO_NOSIGPIPE` does the same job.
- **Who uses this later:**
  - the supervisor (step 3) creates segments and holds the lock files' directory;
  - the engine, workers, and clients map what they're given;
  - step 2's protocol carries descriptors with `send_some()` and `receive_some()`;
  - step 8 uses the lock files to find an engine that outlived its supervisor.
- **What this step settles:** whether a 2 GiB segment really uses memory only for the pages written to it, on macOS as well as Linux. If it doesn't on macOS, ADR 0003 §9's sizing has to change before step 5.
- **The API exists before the code:** the acceptance tests compile against the public headers. Until the maintainer approves this spec and the tests, the function bodies are stubs, and every acceptance test fails.

## Requirements

### Library

1. Everything here goes in `ks::ipc`, which keeps its rule: it depends only on `ks::core`, the standard library, and POSIX, so the supervisor can use it. New public headers: `<ks/ipc/fd.h>`, `<ks/ipc/segment.h>`, `<ks/ipc/fd_passing.h>`, and `<ks/ipc/runtime_dir.h>`.
2. Nothing here runs on the audio thread, so none of it is `KS_NONBLOCKING`. It may allocate and make system calls.
3. **Every descriptor it creates is close-on-exec,** so a process started with `posix_spawn` inherits only the descriptors it's given on purpose (step 3).
4. **System calls interrupted by a signal (`EINTR`) are retried.** Other failures are returned as a `std::error_code` carrying `errno`, never thrown.

### Owning a descriptor

5. **`Fd`** owns one file descriptor and closes it when destroyed. It's move-only, and can give up its descriptor without closing it (`release()`). A default `Fd` owns nothing.

### Creating segments

6. **`SegmentFactory`** creates every segment. `SegmentFactory::open(runtime_dir)` prepares one for a runtime directory (requirement 18).
7. **`create(kind, layout_version, size)`** returns the descriptor of a new segment:
   - its size is `size` rounded up to the page size;
   - it's zero-filled, apart from a header written by `write_header()` with the kind, the layout version, and the rounded size;
   - nothing else can open it: it has no name in the file system or in the shared-memory namespace by the time `create()` returns;
   - it can't change size (requirements 8 and 9).

   A `size` of zero, or one that overflows when rounded up, is refused.
8. **Linux:** the segment is a `memfd`, created with sealing allowed. After writing the header, `create()` adds the `F_SEAL_SHRINK` and `F_SEAL_GROW` seals, then `F_SEAL_SEAL`.
9. **macOS:**
   - the segment is a POSIX shared-memory object;
   - it has a random name of `/kool_spin-` and 16 letters and digits (27 characters), from the system's cryptographic random source;
   - it's created with `O_CREAT | O_EXCL` and mode 0600, sized once with `ftruncate`, then unlinked straight away;
   - if the name is taken (`EEXIST`), `create()` tries a new one, up to 8 times.
10. **The macOS intent file**, `segments.intent` in the runtime directory, stops a crash from leaking a name until reboot:
    - before creating a shared-memory object, `create()` writes its name to the file;
    - after unlinking it, `create()` empties the file;
    - if `create()` fails after the object exists, it unlinks it and empties the file before returning.

    The supervisor is single-threaded, so the file holds at most one name. It isn't synced to disk: shared-memory names don't survive a reboot either.
11. **Recovering on macOS:** `SegmentFactory::open()` holds an exclusive lock on the intent file until the factory is destroyed, and refuses a runtime directory whose intent file is locked by another factory. If the file holds a name of the form `create()` uses, `open()` unlinks it, treating "no such object" as success, and empties the file. Anything else in the file is discarded without unlinking anything.
12. **Linux has no intent file:** `open()` creates no file, and two factories can share a runtime directory.

### Mapping segments

13. **`Region::map(fd, kind, layout_version, access)`** maps a whole segment, read-only or read-write, and checks it in this order:
    1. **Linux:** the segment carries the `F_SEAL_SHRINK`, `F_SEAL_GROW`, and `F_SEAL_SEAL` seals. A descriptor that can't carry seals counts as unsealed.
    2. Its size, from `fstat`, is at least `kSegmentHeaderSize`.
    3. The whole size maps with `MAP_SHARED`.
    4. `check_header()` accepts the mapping for `kind` and `layout_version`, so the header's size must equal the size from `fstat`.

    Each failure is a distinct error, and a failed check leaves nothing mapped. The descriptor isn't closed or kept: the mapping holds the segment alive by itself.
14. **A read-only mapping can't be written:** a write through it crashes the writing process rather than changing the segment.
15. **A segment outlives every process but the last:** it stays valid for as long as any process has its descriptor or a mapping of it, including after the process that created it is killed.

### Passing descriptors

16. **`socket_pair()`** returns two connected Unix stream sockets, both close-on-exec. Writing to a socket whose peer has closed returns `EPIPE`, and never raises `SIGPIPE`: on macOS through `SO_NOSIGPIPE` on both sockets, on Linux through `MSG_NOSIGNAL` in `send_some()`.
17. **`send_some()` and `receive_some()`** are thin wrappers around one `sendmsg` and one `recvmsg`, so the same code works on blocking and non-blocking sockets. Framing whole messages is step 2's job.
    - **`send_some(socket, bytes, fds)`** sends as many of `bytes` as the socket takes, with `fds` attached (`SCM_RIGHTS`), and returns how many bytes it sent. If it sent any, the descriptors went with them. `bytes` must not be empty. It refuses more than `kMaxFdsPerMessage` (8) descriptors without sending anything.
    - **`receive_some(socket, buffer, fds)`** receives up to `buffer.size()` bytes, and returns how many it received: 0 means the peer closed the socket. It appends any descriptors that came with those bytes to `fds` as `Fd`s, each close-on-exec.
    - **Too many descriptors:** if more than `kMaxFdsPerMessage` arrive, or the kernel reports the control data truncated, `receive_some()` closes every descriptor that came with those bytes and returns an error.
    - **Descriptors travel with their bytes:** a receiver that reads fewer bytes than were sent gets the descriptors with the first part.

### The runtime directory

18. **`runtime_directory_path(base)`** works out the runtime directory from `base`, which is `$XDG_RUNTIME_DIR` on Linux and `$TMPDIR` on macOS:
    - if `base` is set, non-empty, and absolute: `<base>/kool_spin`;
    - otherwise, on Linux: `/tmp/kool_spin-<uid>`, with the effective user ID;
    - otherwise, on macOS: `kool_spin` in the per-user temporary directory that `confstr(_CS_DARWIN_USER_TEMP_DIR)` gives.

    `default_runtime_directory_path()` passes the environment variable's value to it.
19. **`open_runtime_directory(path)`** creates the directory with mode 0700 if it doesn't exist. It then refuses it, without changing it, unless it's:
    - a directory, and not a symbolic link;
    - owned by the effective user;
    - inaccessible to the group and to others (no bits in `0077`).

### Lock files

20. **`LockFile::acquire(dir, name)`**:
    - opens or creates `<dir>/<name>.lock` with mode 0600, refusing a symbolic link;
    - takes an exclusive `flock` on it without waiting;
    - replaces its contents with the process ID in decimal and a newline.

    It holds the lock until the `LockFile` is destroyed or the process dies. When another process holds the lock, `acquire()` fails, and reports that process's ID if the file holds a valid one.
21. **`probe_lock(dir, name)`** reports whether a lock file is held, and by which process ID, without waiting:
    - "free" if there's no such file, or nobody holds its lock;
    - "held" with the ID if the file holds a valid one;
    - "held" with no ID if it doesn't.

    A valid ID is a positive decimal number that fits `pid_t`, followed by a newline or nothing. The ID can be trusted only while the lock is held: the holder is alive, so its ID can't have been reused.
22. **Names** are 1 to 64 characters from `a`–`z`, `0`–`9`, `-`, and `_`, so a name can't reach outside `dir`. Other names are refused.
23. **Races:** `probe_lock()` can make an `acquire()` that runs at exactly the same moment fail. Only the supervisor probes, before it starts the process that would acquire, so this doesn't happen in practice.

## Acceptance criteria

The tests are in `tests/acceptance/segments/`. Tests that need more than one process use `fork()`. Each test uses its own runtime directory, in a temporary directory, so tests can run in parallel.

Segments (`segment_test.cpp`):

- [ ] `[auto]` **Creating:** `create()` gives a segment of the size asked for, rounded up to the page size. Mapping it read-write finds a valid header and zeros after it. A size of zero, and one that overflows, are refused. (test: "Segments: created zero-filled with a header")
- [ ] `[auto]` **Passing:** a child forked before the segment exists receives its descriptor over a `socket_pair()`, maps it read-write, and writes 200,000 items through a ring and a seqlock in it. The parent maps the same segment and pops and reads them, with the same checks as Phase 1a's cross-process test. (test: "Segments: shared across processes by descriptor")
- [ ] `[auto]` **Read-only:** a mapping made read-only sees what a read-write mapping of the same segment writes. A child that writes through a read-only mapping doesn't exit normally, and the segment is unchanged. (test: "Segments: a read-only mapping can't be written")
- [ ] `[auto]` **Checks:** `map()` refuses each of these with its own error, and leaves nothing mapped:
  - a segment of another kind;
  - a segment of another layout version;
  - a header whose size differs from the segment's real size;
  - a segment smaller than a header;
  - something that isn't a segment, such as a pipe.

  On Linux it also refuses a `memfd` with no seals, and one sealed against shrinking and growing but missing `F_SEAL_SEAL`. (test: "Segments: mapping checks what it maps")
- [ ] `[auto]` **Fixed size:** resizing a created segment with `ftruncate` fails, both larger and smaller. On Linux the segment carries all three seals. (test: "Segments: can't change size")
- [ ] `[auto]` **Lifetime:** a child creates a segment, writes to it, sends its descriptor, and is killed with `SIGKILL`. The parent maps the segment afterwards, sees what the child wrote, and can write to it. (test: "Segments: a segment outlives its creator")
- [ ] `[auto]` **Lazy memory:** in a fresh child, four segments of 2 GiB plus one page each (a track segment's size, ADR 0003 §9) are created and mapped read-write, and 1 MiB is written to each. The child's peak resident memory grows by less than 256 MiB. (test: "Segments: track-sized segments use memory only for what's written")
- [ ] `[auto]` **The intent file:** (test: "Segments: no names are left behind")
  - On macOS, after `create()` returns, the intent file is empty.
  - On macOS, `open()` unlinks a shared-memory object whose name was left in the intent file, empties the file, and ignores contents that aren't a valid name.
  - On macOS, a second factory for the same runtime directory is refused while the first exists, and accepted after it's destroyed.
  - On Linux, `open()` creates no file in the runtime directory.

Passing descriptors (`fd_passing_test.cpp`):

- [ ] `[auto]` **With their bytes:** three descriptors sent with 100 bytes arrive with the first part when they're received in 10-byte reads. They refer to the same files (`fstat` device and inode), and they're close-on-exec. (test: "Descriptors: travel with the bytes they were sent with")
- [ ] `[auto]` **Limits:** `send_some()` refuses 9 descriptors, and empty bytes, without sending anything. When a raw `sendmsg` delivers 9 descriptors, `receive_some()` returns an error, and the process has no more open descriptors than before the call. (test: "Descriptors: too many are refused and closed")
- [ ] `[auto]` **Closed peer:** after the peer closes, `send_some()` returns `EPIPE`, and the process isn't killed. `receive_some()` returns 0. (test: "Descriptors: a closed peer")

The runtime directory and lock files (`runtime_dir_test.cpp`):

- [ ] `[auto]` **Path:** `runtime_directory_path()` gives `<base>/kool_spin` for an absolute base. For an unset, empty, or relative base, it gives `/tmp/kool_spin-<uid>` on Linux and a `kool_spin` directory in the per-user temporary directory on macOS. (test: "Runtime directory: the path")
- [ ] `[auto]` **Private:** `open_runtime_directory()` creates a missing directory with mode 0700, and accepts it again afterwards. It refuses, and leaves unchanged, an existing directory with mode 0755, a symbolic link to a private directory, and a regular file. (test: "Runtime directory: created private, refused otherwise")
- [ ] `[auto]` **One holder:** while a child holds a lock, `acquire()` in the parent fails and reports the child's process ID, and `probe_lock()` reports it held by that ID. A lock that was never taken probes as free. (test: "Lock files: one holder at a time")
- [ ] `[auto]` **Death releases:** after the holder is killed with `SIGKILL`, `probe_lock()` reports the lock free, and `acquire()` succeeds. (test: "Lock files: released when the holder dies")
- [ ] `[auto]` **Untrusted contents:** when the holder's file holds something other than a valid ID (empty, letters, zero, negative, too large, or more than one line), `probe_lock()` reports it held with no ID. Invalid names, such as `../x`, `a/b`, an empty name, and 65 characters, are refused. (test: "Lock files: contents and names are checked")

Everywhere:

- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux, in every preset, and on macOS arm64 in `debug`, `release`, `asan`, and `tsan`.

## Out of scope

- The socket protocol: message framing, types, the parser and its fuzz target, and checking a client's user ID (step 2).
- Starting processes and choosing which descriptors they inherit (step 3).
- The layouts inside the engine state, track, and log segments (steps 3 to 5), and locking track memory (step 5).
- Finding and stopping an engine that outlived its supervisor (step 8). This step provides only the lock files.
- Fuzzing: the only outside input parsed here is a lock file's process ID and the intent file's name. Both are checked by the tests above. ADR 0003 §8's fuzz targets come in later steps.
- Windows.

## Open questions

None. The maintainer approved these decisions:

- **Runtime directory fallbacks** (requirement 18): ADR 0003 names only `$XDG_RUNTIME_DIR` and `$TMPDIR`. When they're unusable, this spec falls back to `/tmp/kool_spin-<uid>` on Linux and the per-user temporary directory on macOS. The checks in requirement 19 make `/tmp` safe: a directory that another user created first is refused.
- **The intent file holds one name and isn't synced** (requirement 10), because the supervisor is single-threaded and shared-memory names don't survive a reboot.
- **Up to 8 descriptors per message** (requirement 17).
- **The lazy-memory threshold** (256 MiB against 8 GiB) leaves room for sanitizer shadow memory, and still fails clearly if pages are committed up front.
