# Feature: Shared-memory primitives

- **Status:** Draft. Once the maintainer approves it, this file is protected: build loops can't change it.
- **Roadmap:** Phase 1a
- **Built by:** interactive

## Goal

Build the primitives that every kool_spin process uses to talk to the engine: a single-producer, single-consumer ring for commands, a seqlock for state, and a region of shared memory to hold them. They follow ADR 0003's layouts and rules from the start, so Phase 1b only changes what backs the region.

## Context

- **ADR 0003 sections that apply:** §4 (commands in), §5 (state out), §7 (shared-memory segments), and §8 (reading shared memory). The real-time rules in `CLAUDE.md` also apply.
- **What exists:** `ks::core` provides `KS_NONBLOCKING`. There's no IPC code yet.
- **The region in Phase 1a:** anonymous shared memory, from `mmap` with `MAP_SHARED | MAP_ANONYMOUS`. A child created with `fork()` shares it, so tests can exercise the primitives across real processes before Phase 1b brings memfd, `shm_open`, and passing descriptors between processes.
- **Untrusted writers:** whoever writes to shared memory might be buggy or malicious. The primitives validate the counters they read. Validating the payload (a command, a state block) is the caller's job, after the primitive has copied it out.
- **What builds on this:** later Phase 1a steps add the command slot type and its decoder, and the resume state's two slots and CRC.
- **The API exists before the code:** the acceptance tests compile against the public headers. Until the maintainer approves this spec and the tests, the function bodies are stubs, and every acceptance test fails except the compile-time layout checks. The stress tests fail at their 5-second stall limit; the rest fail at once.

## Requirements

### Library

1. A new library, `ks::ipc`, in `src/ipc/`, with public headers `<ks/ipc/layout.h>`, `<ks/ipc/region.h>`, `<ks/ipc/spsc_ring.h>`, and `<ks/ipc/seqlock.h>`. It depends only on `ks::core`, the standard library, and POSIX, so the supervisor can use it too.

### Shared layouts

2. The structs that live in shared memory (`SpscRing`, `Seqlock`, `SegmentHeader`) are standard-layout and trivially copyable, and hold fixed-width integers and arrays of them (plus the ring's slot type), never pointers. Zero-filled memory is a valid empty ring, and a valid seqlock holding a zero value.
3. Words that two processes access at the same time are plain `std::uint64_t`. They're only read and written through `std::atomic_ref<std::uint64_t>`, which must be always lock-free.
4. Fields written by different processes are at least `kSharedAlignment` (128) bytes apart. Apple Silicon's cache lines are 128 bytes, and x86 fetches 64-byte lines in adjacent pairs.

### Ring

5. **`SpscRing<T, Capacity>`** is the shared part of a ring:
   - a head: the number of items pushed, written only by the producer;
   - a tail: the number of items popped, written only by the consumer;
   - `Capacity` slots, with item n in slot n % `Capacity`.

   `T` must be trivially copyable and `Capacity` a power of two; both are checked at compile time.
6. **`RingProducer<T, Capacity>`** attaches to a ring and continues from its head. `try_push(const T&)` copies the value into the next slot, then publishes the new head with release ordering. If the ring is full or corrupt, it returns false and changes nothing.
7. **`RingConsumer<T, Capacity>`** attaches to a ring and continues from its tail. `try_pop()` works in this order:
   1. loads the head with acquire ordering;
   2. copies the oldest slot into private memory;
   3. publishes the new tail with release ordering;
   4. returns the copy.

   If the ring is empty or corrupt, it returns nothing.
8. **Private counters.** Each side keeps its own counter in private memory: the producer its head, the consumer its tail. It reads that counter from shared memory only when attaching, and after that only writes it.
9. **Corruption.** A ring is corrupt when `head − tail`, in unsigned 64-bit arithmetic, is greater than `Capacity`.
   - The consumer checks this when attaching and on every `try_pop()`.
   - The producer checks it when attaching and whenever it reads the tail.
   - A side that finds its ring corrupt never reads a slot or writes its counter again, and its `corrupt()` returns true.
10. `try_push()`, `try_pop()`, and `corrupt()` are `noexcept KS_NONBLOCKING`.

### Seqlock

11. **`Seqlock<T>`** is the shared part of a seqlock: a sequence number that is odd while a write is in progress, and `T`'s bytes as 64-bit words. `T` must be trivially copyable.
12. **`SeqlockWriter<T>`** is the seqlock's only writer.
    - It attaches to a seqlock and continues from its sequence number. If a previous writer stopped mid-write, leaving the number odd, it rounds the number up to the next even one.
    - `write(const T&)` follows ADR 0003 §5:
      1. store sequence + 1 (relaxed);
      2. release fence;
      3. store the words (relaxed);
      4. store sequence + 2 (release).
13. **`SeqlockReader<T>::try_read(max_attempts)`** makes up to `max_attempts` attempts. Each attempt:
    1. loads the sequence (acquire);
    2. loads the words (relaxed) into a local copy;
    3. issues an acquire fence;
    4. loads the sequence again (relaxed).

    It returns the copy from the first attempt in which both loads saw the same even number. If no attempt succeeds, it returns nothing. It never waits, sleeps, or yields.
14. `write()` and `try_read()` are `noexcept KS_NONBLOCKING`.

### Region and segment header

15. **`Region::create_anonymous(size)`** maps zero-filled memory that a child created with `fork()` shares. The mapping is at least `size` bytes, rounded up to the page size. It returns an error code if `size` is zero or the mapping fails. A `Region` is move-only, and unmaps its memory when destroyed.
16. **`Region::at<T>(offset)`** returns the `T` at `offset`. It returns null if:
    - the `T` wouldn't fit inside the region, including when `offset + sizeof(T)` overflows;
    - `offset` isn't aligned for `T`.
17. **Every segment starts with a `SegmentHeader`,** which occupies `kSegmentHeaderSize` (128) bytes. It holds:
    - a magic number;
    - a layout version;
    - a kind (`SegmentKind`: engine state, command ring, track, or log);
    - the segment's size.

    Layouts in the segment start at `kSegmentHeaderSize` or later.
18. **`write_header()` and `check_header()`.** `write_header()` fills in a header. `check_header()` copies the header out of the segment before checking it, and fails with a distinct error in each of these cases:
    - the segment is smaller than `kSegmentHeaderSize`;
    - the magic number doesn't match;
    - the layout version doesn't match;
    - the kind doesn't match;
    - the size doesn't match.

    The size it checks against is the size of the caller's own mapping.

## Acceptance criteria

The tests are in `tests/acceptance/shm-primitives/`. The stress tests fail, rather than hang, when nothing makes progress for 5 seconds.

Ring (`ring_test.cpp`):

- [ ] `[auto]` One producer thread and one consumer thread pass 1,000,000 64-byte items through an 8-slot ring. Each item arrives once, in order, and intact. (test: "Ring: every item arrives once, in order, across threads")
- [ ] `[auto]` A full ring refuses items, an empty ring returns nothing, and items keep their order as the ring wraps around. (test: "Ring: full and empty rings, and wrapping around")
- [ ] `[auto]` A head exactly `Capacity` ahead of the tail is a full ring, not corruption. When a consumer finds the head more than `Capacity` ahead of its tail, or behind it:
  - it returns nothing;
  - it reports the ring corrupt;
  - it stops writing the tail;
  - it still reports corruption after the head is put back.

  (test: "Ring: a consumer rejects a corrupt head")
- [ ] `[auto]` A producer that finds a corrupt tail, when attaching or when the ring is full, refuses to push and reports the ring corrupt. (test: "Ring: a producer rejects a corrupt tail")
- [ ] `[auto]` A new consumer continues from the tail in shared memory, as a restarted engine would. A new producer likewise continues from the head. (test: "Ring: a new consumer or producer continues where the last one stopped")

Seqlock (`seqlock_test.cpp`):

- [ ] `[auto]` One writer thread writes a 256-byte value at least 200,000 times, while two reader threads each read it successfully at least 1,000 times during the writes. No reader ever sees a torn value, or an older value than one it saw before. (test: "Seqlock: readers never see a torn value across threads")
- [ ] `[auto]` A seqlock that has never been written reads as zero. (test: "Seqlock: never written reads as zero")
- [ ] `[auto]` A seqlock left mid-write (odd sequence) reads as nothing after the given number of attempts, without hanging. A new writer makes it readable again. (test: "Seqlock: a write left half-done")

Region and segment header (`region_test.cpp`):

- [ ] `[auto]` A region is zero-filled, a whole number of pages, and shared with a forked child. A region of size zero is refused. (test: "Region: anonymous memory is zero-filled and shared with a forked child")
- [ ] `[auto]` A ring and a seqlock in one region work across processes. A forked child pushes 200,000 items and writes the seqlock after each one. The parent pops and reads, with the same checks as the thread tests. (test: "Region: a ring and a seqlock work across processes")
- [ ] `[auto]` `Region::at()` returns the right address for objects that fit. It refuses objects that don't fit, offsets that overflow, and misaligned offsets. (test: "Region: at() refuses what doesn't fit")
- [ ] `[auto]` `check_header()` accepts a header that `write_header()` wrote, and reports each kind of mismatch, including a kind that isn't a `SegmentKind` at all. (test: "Segment header: mismatches are refused")

Layouts and real-time safety (`layout_test.cpp`):

- [ ] `[auto]` These checks run at compile time (test: "Layouts: shared structs follow ADR 0003"):
  - the shared structs are standard-layout and trivially copyable;
  - `std::atomic_ref<std::uint64_t>` is always lock-free;
  - the ring's head, tail, and slots are at least 128 bytes apart;
  - the header fits in `kSegmentHeaderSize`.
- [ ] `[auto]` Every ring and seqlock operation is called from a `KS_NONBLOCKING` function, so function effect analysis checks them at compile time and RealtimeSanitizer at run time. (test: "Ring and seqlock operations are real-time safe")

Everywhere:

- [ ] `[auto]` All of the above pass in every CI configuration: every preset on Linux, and `debug`, `release`, `asan`, and `tsan` on macOS arm64. That covers Phase 1a's exit criterion for stress tests on arm64.
- [ ] `[manual]` The stress tests catch real ordering bugs. On a throwaway branch, CI on macOS arm64 fails with each of these changes:
  - (a) the acquire fence removed from the seqlock reader;
  - (b) the ring's head published with relaxed instead of release ordering.

  (b) should also fail under `tsan` on Linux. If either change passes, the tests need strengthening before this feature is done.

## Out of scope

- memfd, `shm_open`, sealing, passing descriptors between processes, and the intent file (all Phase 1b).
- The command slot type and its decoder, and the resume state's two slots and CRC (later Phase 1a steps).
- More than one producer per ring.
- Blocking until data arrives (futexes, condition variables). Readers poll.
- Log rings (Phase 1b).
- Fuzzing: these primitives don't parse anything. Fuzz targets come with the command decoder and the resume state.

## Open questions

1. **`std::atomic` or `std::atomic_ref`?** ADR 0003 contradicts itself here:
   - §5 describes the seqlock payload as `std::atomic<std::uint64_t>`;
   - §7 requires shared structs to be trivially copyable, which `std::atomic` isn't.

   This spec uses plain `std::uint64_t`, accessed only through `std::atomic_ref` (requirement 3). That satisfies both sections in substance, and keeps zero-filled memory a valid value without constructing anything in shared memory. If approved, §5's wording changes to match. AppleClang 17's libc++ must provide `std::atomic_ref`; CI on macOS will confirm it does.
