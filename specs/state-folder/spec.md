# Feature: State folder and locks (step 9a)

- **Status:** Approved by the maintainer on 2026-11-25. Protected: build loops can't change it.
- **Roadmap:** Phase 1b, step 9a
- **Built by:** interactive

## Goal

A relaunch finds an engine from an earlier session even when its runtime directory is a different one, or has been removed. The supervisor and the engine each hold a lock in a fixed, permanent folder as well as in the runtime directory. A problem with that folder never stops the music, or keeps it from starting.

## Context

- **ADRs that apply:** ADR 0003 §12 (the engine's lock is how a survivor is found), §14 (the journal's folder), §6 (the runtime directory). ADR 0002 §1 (the supervisor depends on the standard library only).
- **What exists:**
  - **Locks:** `kool_spin` holds `supervisor.lock`, and the engine `engine.lock`, in the runtime directory. The directory comes from `XDG_RUNTIME_DIR` (Linux) or `TMPDIR` (macOS), and isn't permanent.
  - **`specs/supervisor-crash/spec.md`:** finding and stopping an orphan. `find_orphan()` and `stop_orphan()` take the folder as a parameter. Its "Out of scope" lists the ways a runtime directory changes or disappears; this spec is the fix it promises.
  - **The log folder** (`specs/logs/spec.md` requirement 9): on Linux, it's `logs` inside the folder this spec uses.
- **The maintainer's design choices** (2026-11-24 and 2026-11-25): the locks go in the journal's folder; step 9 is split into 9a (this spec) and 9b (the journal); a state folder on FUSE is used only when `--state-dir` names it (requirement 4).
- **What this spec changes:** step 8 decided to *move* the locks. This spec keeps the runtime directory's locks, which stay mandatory, and *adds* a best-effort lock in the state folder. The maintainer approved this with the spec. Why:
  - **The runtime directory is in memory on Linux; the state folder is on disk.** `LockFile::acquire()` empties the file, then writes the process ID. On a full disk the write fails (`ENOSPC`), and on a home directory remounted read-only the open fails (`EROFS`). A moved lock would then fail an engine *restart*, in the middle of a set, and the music would stop for good. A DJ recording a set can fill a disk.
  - **The runtime directory still needs a lock of its own:** `Listener::open()` removes a socket file it finds, and `SegmentFactory::open()` clears the intent file on macOS, because the caller holds the supervisor's lock. Two supervisors with different state folders and one runtime directory would break each other.
  - **`flock` on a network home directory** can fail, or be held by another machine.
- **This spec amends:** `specs/processes/spec.md` requirements 8, 9, 13, and 15; `specs/supervisor-crash/spec.md` requirements 6, 13, and 18; `specs/ks-play/spec.md` (the options and the hint). Everything else in them still applies.
- **Terms:** the *state folder* is the journal's folder of ADR 0003 §14.

## Requirements

### The state folder

1. **`ks::ipc::state_directory_path(home, xdg_state_home, account_home)`** (`<ks/ipc/state_dir.h>`) works out the path. Each value may be missing, and one that isn't an absolute path counts as missing:
   - **Linux:** `<xdg_state_home>/kool_spin`, or else `<home>/.local/state/kool_spin`, or else the same under `account_home`;
   - **macOS:** `<home>/Library/Application Support/kool_spin`, or else the same under `account_home`.

   With none, there's no path. `default_state_directory_path()` passes `HOME`, `XDG_STATE_HOME`, and the home directory that the user database gives for the effective user ID (`getpwuid_r`), which it reads only if it's needed. A launch without an environment, as from cron or a service, so gets the same folder as one from a terminal.
2. **Which folder a process uses:**
   - `--state-dir <path>`, if given;
   - otherwise, with `--runtime-dir`, none: an explicit runtime directory is an instance of its own, as tests are;
   - otherwise, the default.

   The choice records whether `--state-dir` named the folder, for requirement 4. A folder that comes from `XDG_STATE_HOME` is a default one.
3. **`ks::ipc::open_state_directory(path, named)`**, where `named` says whether `--state-dir` named the folder, creates the folder and any missing parents, each with mode 0700. Without changing it, it refuses one that isn't a directory, isn't owned by the effective user, or can be written by the group or by others: the process IDs in its lock files are signaled. Unlike the runtime directory, it may be a symbolic link, and others may read it.
4. **Network file systems and FUSE:** it also refuses a folder that may be shared with another machine, with `std::errc::not_supported`. There, a lock may be held on the other machine, or its file may hold a process ID from it, which would name an unrelated process on this one.
   - **Linux, network types:** a folder for which `statfs` reports the type of NFS, SMB, SMB2, CIFS, AFS, Coda, 9P, or Ceph is refused, however it was chosen.
   - **Linux, FUSE:** refused unless `--state-dir` named it. `statfs` reports one type for every FUSE file system, local or not, and a local one can have a network folder underneath. With `--state-dir`, the user vouches that no other machine shares the folder. The warning (requirement 8) says so, and names `--state-dir` as the fix.
   - **macOS:** a folder for which `statfs` reports no `MNT_LOCAL` is refused, however it was chosen. FUSE has no rule of its own there.

### Locks

5. **Two locks each:** the supervisor and the engine take their lock in the runtime directory, as today, and then, if there is a state folder, the lock of the same name in it. They hold both until they exit.
6. **The same folder:** if the state folder and the runtime directory are one directory, by device and inode, whatever the paths, there is only one lock.
7. **Held by another process:** either lock being held stops a launch, as the runtime directory's does today:
   - **the supervisor:** status 1, and its line gives the holder's process ID and, for the state folder's lock, says that it runs in another runtime directory;
   - **the engine:** status 1, "another engine is running".
8. **Anything else that goes wrong with the state folder** (no path, refused by requirements 3 and 4, or a lock that can't be opened, taken, or written) is a warning, and the process carries on with the runtime directory's lock alone. The supervisor warns in `kool_spin.log` and on standard error, and the engine in its log. A disk that is full or read-only can't stop an engine from starting or restarting.
9. **`LockFile::acquire()` doesn't change.** A lock it couldn't write its process ID to isn't held.

### Command lines

10. **`kool_spin --state-dir <path>`** and **`ks-engine --state-dir <path>`.** Given twice, each is a usage error. `--help` lists it. `kool_spin` passes it to every engine it starts, if it uses a state folder and has opened it.
11. **`ks-play --state-dir <path>`,** with the rules of requirement 2, for requirement 14. Given twice, or empty, it's a usage error.

### Finding the orphan

12. **`kool_spin` looks in the state folder first,** then in the runtime directory:
    1. it takes both `supervisor` locks;
    2. it calls `find_orphan()` on the state folder, and, if that finds none or one whose process ID isn't known, on the runtime directory;
    3. the first orphan with a process ID is the one it offers to stop, with that folder; one without is reported as unknown only if neither folder gives an ID.

    It looks in a state folder it couldn't take its own lock in, if it can read it: the disk may have filled up since the orphan took its lock. It never looks in one that requirements 3 and 4 refuse, because the process IDs there can't be trusted. A failed probe of the state folder is a warning; one of the runtime directory ends the launch, as today.
13. **After stopping it,** it looks in both folders again. Another orphan, which only damage or a change of folders could leave, ends the launch with status 1, and its process ID. The engine's own locks still catch one that appears later.
14. **`ks-play`'s hint** (step 8's requirement 18) probes both folders: a `supervisor` lock held in either means no hint; otherwise an `engine` lock held in either gives it, with the process ID from the first that has one. It applies step 8's checks to each folder, and requirement 4 to the state folder, and never creates one.

### Everything else

15. **Nothing runs on the audio thread,** and a running engine never touches the state folder: only starting one does. No new messages, dependencies, or shared-memory layouts.
16. **The ADR:** ADR 0003 §12 says that there are two locks, why, and which is mandatory; §14 says what the folder is without `HOME`, and that it isn't used on a network file system, or on FUSE unless `--state-dir` names it.

## Acceptance criteria

The tests are in `tests/acceptance/state-folder/`. Each uses runtime, state, and log directories of its own, and never the user's. Tests that start `kool_spin` can't run in the build-loop sandbox (`CLAUDE.md`).

The folder (`state_dir_test.cpp`):

- [ ] `[auto]` **The path:** every combination of missing, relative, and absolute values gives what requirement 1 says, on each platform; the default isn't empty on a machine whose user has a home directory, with `HOME` and `XDG_STATE_HOME` unset. (test: "State folder: the path")
- [ ] `[auto]` **Opening it:** missing parents are created with mode 0700; a file, a folder that the group or others can write, and a dangling link are refused and unchanged; a link to a good folder, and a folder others can read, are accepted. (test: "State folder: created private, refused otherwise")
- [ ] `[auto]` **Network file systems and FUSE:** on Linux, the function that judges a `statfs` result accepts ext4, btrfs, XFS, and tmpfs, and refuses each network type in requirement 4, whether or not `--state-dir` named the folder; it accepts FUSE only if it did. On macOS, it goes by `MNT_LOCAL` alone. (test: "State folder: network file systems and FUSE")
- [ ] `[auto]` **The choice:** each combination of `--state-dir` and `--runtime-dir` gives the folder requirement 2 says, and only `--state-dir` counts as naming it, even for the default's path. (test: "State folder: which one, and whether it was named")

Locks (`state_locks_test.cpp`):

- [ ] `[auto]` **Both held:** while `kool_spin --state-dir` runs, both folders' locks name it and its engine; after it stops, all four are free. After `kill -9` of the engine, the new engine holds both. (test: "State locks: both folders")
- [ ] `[auto]` **One folder:** with the state folder the same directory as the runtime directory, by the same path and through a symbolic link, `kool_spin` starts, and stops cleanly. (test: "State locks: one folder for both")
- [ ] `[auto]` **A second supervisor:** with the same state folder and another runtime directory, it exits with status 1 and the first's process ID; with another state folder and the same runtime directory, the same, and the first's socket still answers. (test: "State locks: a second supervisor")
- [ ] `[auto]` **Which folder:** `--runtime-dir` alone creates nothing outside it and the log folder; with neither option, and `HOME`, `XDG_STATE_HOME`, `XDG_RUNTIME_DIR`, and `TMPDIR` pointing into the test's directory, the locks are where requirement 1 says. (test: "State locks: which folder")
- [ ] `[auto]` **A bad folder never stops the music:** for a state folder that is a file, one that others can write, and one in a directory without write permission, `kool_spin` warns, starts, loads a track, plays it, and restarts a killed engine. The same when the folder becomes unwritable, and its `engine.lock` unopenable, while the track plays and the engine is then killed. (Skipped as root, whom permissions don't stop.) (test: "State locks: a bad folder never stops the music")
- [ ] `[auto]` **A lock that can't be written:** in a child process whose file-size limit is 0 (`RLIMIT_FSIZE`, with `SIGXFSZ` ignored), standing in for a full disk, `acquire()` on an existing lock file fails, and the lock is then free. At process level, an existing `engine.lock` in the state folder with mode 0400 makes `ks-engine` warn and reach `Ready`. (The limit can't be used there: it would fail the runtime directory's lock too.) (test: "State locks: a lock that can't be written")
- [ ] `[auto]` **Held by another process:** the test holds the state folder's `engine` lock: a `ks-engine` run directly exits with status 1 and the holder's process ID. (test: "State locks: an engine lock held elsewhere")
- [ ] `[auto]` **Options:** `--state-dir` twice is a usage error with status 2 in all three programs, and `--help` lists it. (test: "State locks: options")

The orphan (`state_orphan_test.cpp`), after `SIGKILL` to a `kool_spin` that plays a track:

- [ ] `[auto]` **Another runtime directory:** a launch with the same state folder and another runtime directory exits with status 1 and the orphan's process ID, and the heartbeat keeps increasing; with `--stop-previous`, the recording ends in a fade, and a new session starts in the new directory. (test: "State orphan: another runtime directory")
- [ ] `[auto]` **A removed runtime directory:** the same, once the orphan's runtime directory has been deleted. (test: "State orphan: a removed runtime directory")
- [ ] `[auto]` **A lost state lock:** with the state folder's `engine.lock` deleted, and with the folder never usable, the runtime directory's lock still finds the orphan. With the state folder's file emptied while held, the runtime directory's gives the process ID. (test: "State orphan: found through the runtime directory")
- [ ] `[auto]` **A state folder that can't be written but can be read:** the orphan is still found through it, from another runtime directory. (test: "State orphan: a read-only state folder")
- [ ] `[auto]` **A refused folder is never searched:** with the orphan's state folder made writable by others, a launch from another runtime directory doesn't find it, warns about the folder, and starts; one from the orphan's own runtime directory finds it there. (test: "State orphan: never found in a folder that's refused")
- [ ] `[auto]` **Two orphans:** with the test's helper holding the runtime directory's `engine` lock, and an orphan the state folder's, `--stop-previous` stops the orphan, and exits with status 1 and the helper's process ID. (test: "State orphan: two of them")
- [ ] `[auto]` **The hint:** `ks-play` gives it for an orphan found only in the state folder, and not while a supervisor holds either lock, nor from a state folder that others can write; it creates no file or folder. (test: "State orphan: ks-play's hint")

Everywhere:

- [ ] `[auto]` **The earlier steps' tests** all pass, unchanged.
- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux in every preset, and on macOS arm64 in `debug`, `release`, `asan`, and `tsan`.
- [ ] `[manual]` **By hand:** the maintainer plays a track, kills `kool_spin` with `kill -9`, and launches it again with `env -u XDG_RUNTIME_DIR`: it finds the engine and offers to stop it.
- [ ] `[manual]` **On FUSE** (Linux, with `XDG_STATE_HOME` in a gocryptfs or similar mount, because CI can't count on mounting one): `kool_spin` warns, names `--state-dir`, starts, and leaves the folder's locks alone; with `--state-dir` naming that same folder, there is no warning, and both folders' locks are held.

## Out of scope

- **The session journal** (step 9b), whose writer needs the state folder's `supervisor` lock or a decision about doing without it.
- **An orphan with another state folder:** one with another `XDG_STATE_HOME` or `HOME`, or another `--state-dir`, *and* another runtime directory. These are set far less often, and by the user.
- **An orphan of an earlier build,** which holds only the runtime directory's lock. It's found there, as today.
- **A state folder on a network file system, or on FUSE without `--state-dir`:** only the runtime directory's locks are used, with a warning. In 9b, such a session also has no journal, unless that spec decides otherwise.
- **An orphan whose state folder is on FUSE,** when the relaunch doesn't give the same `--state-dir`: it's found only through the runtime directory.
- **Telling FUSE file systems apart** by the subtype in `/proc/self/mountinfo`: the FUSE program chooses the name, and it says nothing about what's underneath.
- **Other instances on purpose:** `--runtime-dir` with no `--state-dir` still starts a second, separate `kool_spin`.

## Open questions

None. The maintainer approved these on 2026-11-25, with the spec:

1. **Adding locks rather than moving them,** with the state folder's as best effort, because a moved lock would make an engine restart depend on a writable disk.
2. **`--runtime-dir` without `--state-dir` uses no state folder** (requirement 2), so every existing test stays as it is, and in 9b none of them writes or restores a journal.
3. **FUSE** is used only when `--state-dir` names the folder (requirement 4).
4. **The edits to ADR 0003** §12 and §14 (requirement 16).
