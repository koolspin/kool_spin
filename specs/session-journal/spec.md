# Feature: Session journal (steps 9b and 9c)

- **Status:** Approved by the maintainer on 2026-11-25. Protected: build loops can't change it.
- **Roadmap:** Phase 1b, steps 9b and 9c
- **Built by:** interactive

## Goal

The supervisor saves the session (each deck's file, position, rate, and cue point, and the mixer) to a journal in the state folder, at most a second behind the engine. At launch, it restores the session from the journal with decks paused, before the engine starts, so that a set can carry on after any crash, a reboot, or a power cut. Neither writing nor restoring can stop the music, or keep it from starting.

## Context

- **ADRs that apply:** ADR 0002 §6 (the journal: atomic, at least once a second; restored paused), ADR 0003 §14 (content, format, location, restoring), §6 (the supervisor is single-threaded; this spec adds a writer thread), §11 (resume slots and `--track`), §12 (the orphan, and the locks). ADR 0002 §1: the supervisor depends on the standard library only.
- **What exists:**
  - **The state folder** (`specs/state-folder/spec.md`): `kool_spin` chooses it, opens it, and takes its `supervisor` lock there, best effort. `--runtime-dir` without `--state-dir` means none, so no existing test writes or reads a journal.
  - **Loads** (`specs/track-loading/spec.md`): the supervisor opens each file, starts a worker, which probes it, creates the track segment, and sends `LoadTrack`. It knows each track's path. Paths are absolute, valid UTF-8, at most 4,096 bytes.
  - **Restarts** (`specs/engine-restart/spec.md`): the engine resumes from the newer valid resume slot, given the tracks as `--track <id>:<fd>`. A deck whose track ID isn't among them comes back empty. The resume slots are validated by `engine::validate()` (`specs/resume-state/spec.md` requirement 7).
  - **The orphan** (`specs/supervisor-crash/spec.md`): a relaunch can stop an engine whose supervisor died. Until then, it played on, and the journal stopped being written.
- **The maintainer's design choices** (2026-11-25): a writer thread writes the journal; restoring happens before the engine starts: the supervisor probes the files in up to 4 workers, waits about 3 s at most, writes a resume slot with every deck paused, and starts the engine with the tracks as `--track`; after stopping an orphan, decks that were playing are moved on by the time that passed, by the monotonic clock.
- **Two steps, one spec** (the maintainer's choice, 2026-11-25), each its own pull request:
  - **9b, writing:** requirements 1 to 16, 30's `--journal-interval`, 31, 32, and 33's ADR 0003 §6 and the format and writing parts of §14 and ADR 0002 §6; the criteria under "The format" and "Writing". In 9b there are no carried entries yet.
  - **9c, restoring:** requirements 17 to 29, 30's `--new-session`, and the rest of 33; the criteria under "Restoring", and the manual ones.
- **This spec amends:** `specs/processes/spec.md` requirements 13 and 15 (options, and the start sequence); `specs/supervisor-crash/spec.md` requirement 11 (the time the orphan is stopped is recorded). Everything else in them still applies.
- **Terms:**
  - an *entry* is one deck's part of the journal;
  - a *carried* entry is one the supervisor couldn't restore, and keeps in the journal (requirement 24);
  - the *journal* is `<state folder>/session.journal`.

## Requirements

### Limits shared by the engine and the supervisor

1. **The ranges a resume slot's values must fall in move to `<ks/ipc/state.h>`,** beside the layout: `ipc::kMaxDeckRate` (4), `ipc::kMaxTrimDb` (12 dB), `ipc::kMinCrossfaderTransition` (0.01), and `ipc::kMaxOutputChannels` (8). The engine's `kMaxRate`, `kMaxTrimDb`, `kMinCrossfaderTransition`, and `kMaxOutputChannels` become aliases of them, as `engine::kMaxTrackFrames` is of `ipc::kMaxTrackFrames`. Nothing else in the engine changes.

### The format (`<ks/supervisor/journal.h>`)

2. **`Journal`** holds:
   - `boot`: the boot ID of the machine when it was written (requirement 15), or none;
   - `engine`: the process ID of the engine whose state it records, or 0 for none;
   - `time`: the steady clock's time of that state, in nanoseconds since its epoch;
   - `decks`: for each of the 4 decks, an entry or nothing. An entry has the file's `path`, `size` in bytes, and modification time (`modified`, in nanoseconds since the Unix epoch, which may be negative), the track's `sample_rate`, and the deck's `position` and `cue` (in track frames), `rate`, and whether it was `playing`;
   - `mixer`: every setting of the engine's mixer, as `MixerState` has them, the bus map included.
3. **`format(const Journal&)`** writes it as text: lines ending in `\n`, each `key value`, in exactly this order:

   ```
   kool_spin session journal 1
   boot <id>
   engine <pid>
   time <ns>
   deck <n> path <path>
   deck <n> size <bytes>
   deck <n> modified <ns>
   deck <n> sample-rate <hz>
   deck <n> position <frames>
   deck <n> rate <rate>
   deck <n> cue <frames>
   deck <n> playing <0|1>
   channel <n> trim-db <dB>
   channel <n> fader <0..1>
   channel <n> side <a|b|thru>
   channel <n> cue <0|1>
   crossfader <0..1>
   crossfader-transition <0.01..1>
   main-level-db <dB>
   headphone-level-db <dB>
   cue-mix <0..1>
   bus-main <channel>
   bus-cue <channel>
   checksum <crc>
   ```

   - **Decks** are numbered from 1. The eight `deck` lines appear for each deck with an entry, decks in order; a deck without one has none. The four `channel` lines appear for channels 1 to 4, in order.
   - **`boot`** is 1 to 64 characters from `0-9`, `a-f`, and `-`, or `none`.
   - **Integers** are decimal, with no leading zeros or `+`; only `modified` may be negative.
   - **Reals** are what `std::to_chars` writes for a `double` in its shortest form that reads back exactly; −∞ is `-inf`. Only trims and levels may be −∞; nothing is NaN or +∞.
   - **Paths** are written as they are, except that `\` becomes `\\`, and each byte from 0x00 to 0x1F, and 0x7F, becomes `\x` and two lowercase hex digits.
   - **`checksum`** is `crc32()` (`<ks/ipc/crc32.h>`) of every byte before the line, as 8 lowercase hex digits.

   So every `Journal` has exactly one text, and the text is readable when debugging.
4. **`parse(std::span<const std::byte>)`** returns the `Journal`, or why not. It accepts only text that `format()` could have written, and only values that requirement 5 allows: a missing, extra, or reordered line, a byte that `format()` wouldn't write, a bad escape, a path that isn't valid UTF-8 or absolute or is longer than 4,096 bytes after unescaping, a wrong checksum, another version, and more than `kMaxJournalSize` (128 KiB) of input are each refused. Accepting is all or nothing: one bad line refuses the journal. It's a pure function of its input, and safe for any bytes. **Round trip:** for any input it accepts, `format()` of the result gives back the same bytes.
5. **Values:** a size from 0 to 2⁶³ − 1; a sample rate from `kMinTrackRate` to `kMaxTrackRate`; positions and cue points finite, from 0 to `kMaxTrackFrames`; rates finite, from −`kMaxDeckRate` to `kMaxDeckRate`; process IDs from 0 to 2³¹ − 1; a time from 0 to 2⁶³ − 1; and mixer settings that `engine::validate()` would accept (`specs/resume-state/spec.md` requirement 7).
6. **`resume_state(const Journal&, track_ids, generation, host_time)`**, where `track_ids` gives a track ID for each of the 4 decks (0 for none), returns the resume slot for it: each deck with an entry and a non-zero track ID gets that ID, the entry's position, rate, and cue point, and is paused; the others are empty, with rate 1; the mixer is the journal's. The checksum is right. For any `Journal` that `parse()` accepts, any track IDs, and any generation from 1 to 2⁶³ − 1, `engine::validate()` accepts the result, for slot `generation % 2`.

### Fuzzing

7. **Under the `fuzz` preset,** a libFuzzer target runs for 200,000 inputs with a fixed seed. For each input, it:
   - parses its bytes: whatever `parse()` accepts formats back to the same bytes, has values requirement 5 allows, and gives a resume slot that `engine::validate()` accepts;
   - does the same with the checksum line corrected, so that the line checks are reached;
   - builds a valid `Journal` from its bytes (paths included, with every byte that needs an escape), and checks that it formats, parses back equal, and formats again to the same bytes.

   The checks live in a header that an ordinary test also runs, on 100,000 inputs from a fixed seed, in every preset.

### Writing

8. **Only with the lock:** the supervisor writes the journal only if it holds the state folder's `supervisor` lock, or the state folder is the runtime directory (one lock). Without either, it warns once, at start, that the session won't be saved. It may still restore (requirement 17).
9. **What it records:** every `--journal-interval` (default 1 s; 1 to 3,600,000 ms), once the engine has said `Ready` and its heartbeat has been seen to be non-zero, the supervisor reads each deck's state block and the mixer's (copy, validate, use), and builds a `Journal`:
   - `boot`: this machine's; `engine`: the engine's process ID; `time`: the status block's host time;
   - **a deck whose track is one the supervisor loaded or restored** gets an entry: the file's path, and the size and modification time it had when the supervisor opened it, the track's sample rate, and the deck's position, rate, cue point, and whether it's playing. A position or cue point outside 0 to `kMaxTrackFrames`, or a rate outside the limits, is clamped to them; one that isn't finite makes the entry 0 (position, cue) or 1 (rate);
   - **a deck with a carried entry** gets that entry, unchanged (requirement 24);
   - **any other deck** has none;
   - the mixer's settings. If they aren't values requirement 5 allows, the last ones that were are used, or the defaults.

   Anything it can't read in this pass is left as it was last time.
10. **Only when changed:** it hands the journal to the writer only if something other than `time` differs from the last journal it handed over. A paused session isn't rewritten.
11. **The writer thread** writes the newest journal it has been given, and drops any older one it hasn't started, so it never falls behind. The poll loop never waits for it, or for the disk. The thread runs with the supervisor's handled signals blocked. It's the only thread the supervisor starts; ADR 0003 §6 says so.
12. **Atomically:** it writes `session.journal.tmp` (mode 0600, created or truncated, not through a symbolic link), syncs it, renames it over `session.journal`, and syncs the folder. On macOS, it syncs with `F_FULLFSYNC`, and falls back to `fsync` if that fails. A crash or power cut at any point leaves the old journal or the new one, complete.
13. **Failures never stop the music:** a write that fails (a full or read-only disk, a folder that has been removed) is a warning that says why, once, until a write succeeds again, which is info. The supervisor keeps trying at each change, or at each interval while the last write failed.
14. **The last write:** when the supervisor stops, it writes the journal once more from the engine's last state blocks, after the engine has exited, and waits up to `kJournalExitTimeout` (2 s) for the writer. If it's still writing then, it says so, and exits without waiting: the journal on disk is then the old one or the new one.
15. **The boot ID:** on Linux, `/proc/sys/kernel/random/boot_id`; on macOS, the `kern.bootsessionuuid` sysctl, in lowercase. If it can't be read, or isn't a valid `boot` value, it's `none`.
16. **Not before the first block:** a launch never writes the journal before its engine's heartbeat is non-zero, so a launch whose engine fails to start leaves the journal as it was. Nor does it write while an engine is restarting.

### Restoring

17. **When:** at launch, after it has stopped an orphan, if there was one, and before it starts the engine, the supervisor restores the session from the journal, if there is a state folder and a journal, unless `--new-session` is given. It restores even without the state folder's lock (requirement 8), if it can read the journal. Restoring can't fail the launch: whatever goes wrong is a warning, and the session starts without what couldn't be restored.
18. **A journal that's refused:** one that isn't a regular file, can't be read, or that `parse()` refuses is a warning that says why, and the session starts with nothing restored. The supervisor renames the journal to `session.journal.rejected` (replacing an older one), so that the new session doesn't overwrite it, and it can be looked at.
19. **Each entry** is restored as a load is, without a client:
    1. the supervisor opens the file, as for `LoadFile`, and reads its size and modification time;
    2. it starts a worker for it, at most `kMaxWorkers` (4) at once, and sends `Probe`;
    3. once probed, it checks the budget, creates the track segment, and sends `Decode`. Decoding goes on after the engine has started, and a deck can be played as soon as what it needs is decoded.
20. **The time limit:** the supervisor waits for the probes for at most `kRestoreProbeTimeout` (3 s) from the first worker's start, running its poll loop (signals and logs). An entry not probed by then isn't restored: its worker is stopped.
21. **Track IDs** come from the session's counter, in deck order, as for loads.
22. **What isn't restored:** an entry whose file can't be opened or isn't a regular file, whose worker refuses it or ends before probing, that isn't probed in time, or that doesn't fit the budget leaves its deck empty, with a warning that names the deck, the path, and why. It's carried (requirement 24).
23. **A changed file:** if the file's size or modification time differs from the entry's, or the probed sample rate does, the track is restored at position 0 and cue point 0, with a warning: positions in the old file may not mean anything in the new one.
24. **Carried entries:** a deck whose entry wasn't restored keeps it in the journal, with `playing` 0, until a client loads or unloads that deck. A later launch then tries it again: a USB drive that wasn't mounted at launch may be by then.
25. **The resume slot:** once every entry is restored or given up, the supervisor writes `resume_state()` of the restored decks to the engine-state segment's slot 1, as generation 1, with the steady clock's time now, through a seqlock writer; and starts the engine, without `--clean`, passing each restored track as `--track <id>:<fd>`. The engine resumes from it as after a restart: decks paused, at their positions, with the journal's mixer settings, and no fade audible, since nothing plays. With nothing to restore, the segment is left zero-filled, as today.
26. **After an orphan:** the decks of an orphan that a launch stopped kept playing after its supervisor died, while the journal stood still. If the journal's `boot` is this machine's, and not `none`, and its `engine` is the orphan's process ID, each entry that was `playing` moves on by `rate × sample rate × elapsed`, where `elapsed` is the time from the journal's `time` until the supervisor sent the orphan `SIGTERM`, or found it already gone, by the steady clock. The position is clamped to 0 and `kMaxTrackFrames`; the engine moves a position past the end of a complete track to its end. Otherwise, no entry moves. Either way, every deck is restored paused.
27. **An engine that fails with a restored session:** if the first engine of a launch that restored anything exits, or doesn't say `Ready` in time, the supervisor says so, and starts one engine clean (`--clean`, no tracks, the segment wiped past its header), with the restored decks' entries carried. If that one fails too, the launch fails, as today.
28. **Stopping while restoring:** `SIGINT`, `SIGTERM`, or `SIGHUP` during restoring stops the workers, leaves the journal as it was, and exits with status 0, without starting an engine.
29. **Reports:** in `kool_spin.log`: info for "restoring the session from <journal>", each deck restored (its number, path, and position in seconds), and "no session to restore"; a warning for each deck not restored or restored at the start, and for a journal refused or not readable. With nothing in the journal to restore, it says so at info.

### Command lines

30. **`kool_spin --new-session`:** don't restore; the journal is replaced once the new engine has rendered a block, as requirement 16 says. **`kool_spin --journal-interval <ms>`:** for tests. Given twice, or out of range, each is a usage error. `--help` lists both.
31. **`kool_spin --journal-stall-after <n>`**, for tests only, and not listed by `--help`: the writer thread, on its `n`th write (1 to 1,000), stops before it opens the file, until the supervisor exits: a disk that hangs.

### Everything else

32. **Nothing runs on the audio thread,** and the engine doesn't touch the journal. No new messages, dependencies, or shared-memory layouts; the engine's only change is requirement 1.
33. **The ADRs:** ADR 0003 §6 says that the supervisor's one other thread writes the journal; §14 gives the file's name, the format of requirement 3, that it's written when something has changed, at most `--journal-interval` behind, restoring before the engine starts, carried entries, changed files, and the move after an orphan. ADR 0002 §6's "at least once a second" becomes "within a second of any change".

## Acceptance criteria

The tests are in `tests/acceptance/session-journal/`. Each uses runtime, state, and log directories of its own, and never the user's. Tests that start `kool_spin` can't run in the build-loop sandbox (`CLAUDE.md`).

The format (`journal_format_test.cpp`):

- [ ] `[auto]` **Limits:** the engine's limits equal the ipc ones, and `engine::validate()` still accepts and refuses at the same bounds. (test: "Journal format: shared limits")
- [ ] `[auto]` **A known journal:** a `Journal` with decks 1 and 3, a −∞ trim, `boot none`, a negative `modified`, and a path with a space, `é`, a backslash, a tab, and a newline formats to the expected text, byte for byte, and parses back equal. An empty session formats with no `deck` lines. (test: "Journal format: a known journal")
- [ ] `[auto]` **Refusals:** each of these is refused, and saying why: every line missing, doubled, and swapped with the next; a wrong checksum, in a hex digit and in case; version 2; a trailing byte, a missing final `\n`, and `\r\n`; integers with a leading zero or `+`; a real that isn't in shortest form (`1.0`), NaN, `inf`, and `-inf` where it's not allowed; each value one step past each bound in requirement 5; `\q`, `\x4`, `\x41` (an escape `format()` wouldn't write), and a raw tab in a path; a relative path, a 4,097-byte one, and one that isn't UTF-8; deck 0 and deck 5; and 128 KiB + 1 bytes. (test: "Journal format: refusals")
- [ ] `[auto]` **The resume slot:** for the known journal, `resume_state()` gives the expected words, every deck paused, and `engine::validate()` returns the expected snapshot; for each bound of requirement 5, it still validates. (test: "Journal format: the resume slot")
- [ ] `[auto]` **Fuzz checks:** the checks of requirement 7 on 100,000 inputs. (test: "Journal format: fuzz checks")
- [ ] `[auto]` **The fuzz target** runs 200,000 inputs under `fuzz` without a failure. (CTest: `ks_journal_fuzz`)

Writing (`journal_write_test.cpp`):

- [ ] `[auto]` **Written:** with `--journal-interval 50`, after loading two tracks and changing rate, cue, and the mixer through a ring, and playing deck 1, the journal parses within 200 ms, with the paths, sizes, and modification times, the deck and mixer values from the state blocks, this boot's ID, the engine's process ID, and mode 0600; `session.journal.tmp` is gone. (test: "Journal write: what it records")
- [ ] `[auto]` **Only when changed:** with everything paused, the journal's inode and modification time stay the same for 10 intervals; a seek rewrites it within 2 intervals; while a deck plays, it's rewritten at least every 2 intervals. (test: "Journal write: only when changed")
- [ ] `[auto]` **Not before the first block:** with a valid journal in place and an engine that never says `Ready` (`-- --start-delay` beyond `--ready-timeout`), and with `--engine` an executable that exits at once, the launch fails and the journal is byte for byte as it was. (test: "Journal write: not before the first block")
- [ ] `[auto]` **Atomic:** in a child process, a write with `RLIMIT_FSIZE` 0 (and `SIGXFSZ` ignored) fails, and leaves the old journal byte for byte. At process level, with `--journal-interval 10` and a deck playing, `kill -9` of `kool_spin` at 20 random times leaves a journal that parses every time. (test: "Journal write: atomic")
- [ ] `[auto]` **A folder that can't be written:** made read-only while a deck plays, the supervisor warns once, the recording has no gap, and an engine killed then restarts and resumes; made writable again, the journal is written within 2 intervals, with an info line. (Skipped as root.) (test: "Journal write: a folder that can't be written")
- [ ] `[auto]` **A writer that hangs:** with `--journal-stall-after 2`, the session keeps going: a ring's commands apply, a load succeeds, a `SIGSTOP`ped engine is killed and restarted, and `ks-play status` answers. On `SIGTERM`, `kool_spin` exits with status 0 within 2.5 s, and says the journal wasn't written. (test: "Journal write: a writer that hangs")
- [ ] `[auto]` **Without the lock:** with the state folder's `supervisor.lock` made mode 0400 beforehand, `kool_spin` warns that the session won't be saved, plays a track, and never changes the journal. (Skipped as root.) (test: "Journal write: without the lock")
- [ ] `[auto]` **The last write:** a seek made 10 ms before `SIGTERM`, with `--journal-interval 60000`, is in the journal after `kool_spin` exits. (test: "Journal write: the last write")
- [ ] `[auto]` **No state folder:** with `--runtime-dir` alone, nothing is written outside the runtime and log directories. (test: "Journal write: no state folder")
- [ ] `[auto]` **Options:** `--journal-interval` twice, `0`, `3600001`, and `x` are usage errors with status 2, as are `--journal-stall-after` `0` and `1001`; `--help` lists `--journal-interval`, and not `--journal-stall-after`. (test: "Journal write: options")

Restoring (`journal_restore_test.cpp`):

- [ ] `[auto]` **After everything is killed:** two tracks loaded, deck 1 playing at rate 1.05 with a cue point, deck 3 paused at 12.5 s, and the mixer changed; after `kill -9` of `kool_spin`, the engine, and the workers, a relaunch with the same state folder has decks 1 and 3 loaded and paused, each within 1 block (at most 1,024 frames) of the journal's position, with its rate and cue point, and the journal's mixer settings; `ks-engine.log` says it resumed from generation 1; each deck plays when asked, and the recording isn't silent. (test: "Journal restore: after everything is killed")
- [ ] `[auto]` **A missing file:** with deck 1's file moved away, deck 1 is empty with a warning naming it, deck 3 is restored, and the journal keeps deck 1's entry, not playing; with the file put back, the next launch restores both. (test: "Journal restore: a missing file is carried")
- [ ] `[auto]` **Carried until the deck is used:** a carried entry stays across 3 launches; loading another file on that deck replaces it; on another launch, unloading the deck removes it. (test: "Journal restore: carried until the deck is used")
- [ ] `[auto]` **A changed file:** a file whose modification time changed, one whose size changed, and one replaced by a file of another sample rate are restored at position 0 and cue point 0, each with a warning. (test: "Journal restore: a changed file starts at 0")
- [ ] `[auto]` **Not restored:** a file the worker refuses (text), a probe that takes 5 s (a fake worker), and a track that doesn't fit `--track-memory 1` each leave the deck empty and carried, with a warning that says why; the launch takes under 4 s; the other decks are restored. (test: "Journal restore: what can't be restored")
- [ ] `[auto]` **A journal that's refused:** for a bad checksum, version 2, 128 KiB + 1 bytes, a directory, and a file mode 0000 (skipped as root), the session starts empty, with a warning that says why; the refused journal is now `session.journal.rejected`, byte for byte, except the directory and the unreadable file, which are left; a new journal is written once the engine renders. (test: "Journal restore: a journal that's refused")
- [ ] `[auto]` **`--new-session`:** nothing is restored, and the journal records the new, empty session within 2 intervals of `Ready`. (test: "Journal restore: --new-session")
- [ ] `[auto]` **After an orphan:** with deck 1 playing at rate 0.9, deck 2 playing in reverse, and deck 3 paused, `kill -9` of `kool_spin`; after 2 s, `kool_spin --stop-previous` restores decks 1 and 2 moved on by the time to its `SIGTERM` (within 100 ms of playing), deck 2 not below 0, and deck 3 where it was. With the journal's `engine` changed to another process ID, or its `boot` to another ID, nothing moves. (test: "Journal restore: after an orphan, decks move on")
- [ ] `[auto]` **An engine that fails with a restored session:** with `--engine` a script that exits with status 1 when given `--track`, and runs `ks-engine` otherwise, the launch warns, starts clean with empty decks, and runs; the journal keeps both entries, carried. (test: "Journal restore: an engine that fails starts clean")
- [ ] `[auto]` **Stopping while restoring:** `SIGTERM` while a fake worker takes 5 s to probe ends `kool_spin` with status 0 within 1.5 s, with no worker left running, no engine started, and the journal as it was. (test: "Journal restore: stopping while restoring")
- [ ] `[auto]` **Without the lock:** with the state folder's `supervisor.lock` made mode 0400, a journal is still restored, and left unchanged. (Skipped as root.) (test: "Journal restore: without the lock")
- [ ] `[auto]` **Options:** `--new-session` twice is a usage error with status 2; `--help` lists it. (test: "Journal restore: options")

Everywhere:

- [ ] `[auto]` **The earlier steps' tests** all pass, unchanged.
- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux in every preset, and on macOS arm64 in `debug`, `release`, `asan`, and `tsan`.
- [ ] `[manual]` **After a reboot** (roadmap exit criterion): the maintainer loads two tracks, sets cue points and the mixer, reboots without stopping `kool_spin`, and launches it again: both decks are back, paused, where they were.
- [ ] `[manual]` **On the Inpulse 500:** a track playing, `kill -9` of every process, and a relaunch restores it paused; play resumes from there.

## Out of scope

- **Content fingerprints:** a file is recognised by its path, size, and modification time until Phase 2.
- **What happened to an orphan's decks after its supervisor died,** other than playing on: a pause, seek, or rate change made through a client's ring then is lost, because a relaunch can't read the orphan's state. Adopting the orphan (ADR 0003 §12) would fix that.
- **Positions for decks more than an orphan's:** after a clean stop, a crash of everything, or a reboot, decks come back where the journal has them, without moving on.
- **A journal on a network file system,** or FUSE without `--state-dir`: there's no state folder, so no journal (`specs/state-folder/spec.md`).
- **Taking the state folder's lock later,** if it couldn't be taken at launch: the session isn't saved until the next launch.
- **Restoring after a clean restart:** a crash loop's clean restart empties the decks, and the journal then records them empty.
- **Older journals:** version 1 is the only one. A later version may convert older ones; this one refuses anything else.
- **`ks-play`:** nothing in it changes. It sees restored decks as it sees any deck.
- **The playing state:** decks are always restored paused, even after an orphan.

## Open questions

None. The maintainer approved these on 2026-11-25, with the spec:

1. **Two steps:** 9b (the format, the fuzz target, and writing) and 9c (restoring), each its own pull request, under this spec.
2. **A changed file** is restored at position 0, with a warning (requirement 23).
3. **A refused journal is kept** as `session.journal.rejected` (requirement 18).
4. **Restoring without the state folder's lock,** if the journal can be read; only writing needs the lock (requirements 8 and 17).
5. **Decks move on after an orphan** only if the journal's boot ID and engine process ID match it (requirement 26).
6. **The last write on stopping,** with a 2 s cap (requirement 14).
7. **ADR 0002 §6** says "within a second of any change" (requirement 33).
8. **Carried entries** (24), **`--new-session`** (30), **one clean retry** for an engine that fails with a restored session (27), and **writing only when something changed** (10).
