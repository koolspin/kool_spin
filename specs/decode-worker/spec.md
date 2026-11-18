# Feature: The decode worker

- **Status:** Approved by the maintainer on 2026-11-17. Protected: build loops can't change it.
- **Roadmap:** Phase 1b, step 5a
- **Built by:** interactive

## Goal

Decode audio files into track segments in a separate, disposable process, `ks-worker`, so a file that crashes or hangs the decoder can't touch the engine. The worker reads a file through a descriptor the supervisor gives it, reports the file's sample rate and estimated length, and then decodes it with FFmpeg into a track segment: interleaved stereo 32-bit float at the file's own rate. It publishes the frames as they're decoded, so a deck can start playing before decoding finishes. Step 5b connects it: the supervisor starts a worker for each load, and the engine plays the segment.

## Context

- **ADRs that apply:**
  - **ADR 0001:** FFmpeg (libavformat, libavcodec) decodes, in worker processes only.
  - **ADR 0002:** workers (`ks-worker`) decode, one job at a time, and are disposable: a crash fails that job and nothing else. Their allowed dependencies are FFmpeg, TagLib, qm-dsp, and libebur128.
  - **ADR 0003 §6:** loading a track goes through the supervisor, which creates the track segment and passes it to a worker and to the engine. Real-time data never passes through the supervisor.
  - **ADR 0003 §7 and §8:** a track segment has one writer, the worker. Readers copy shared values, validate them, and take sizes from their own mapping.
  - **ADR 0003 §9:** track memory is interleaved stereo 32-bit float at the file's rate. Each track is capped at 2 GiB of samples, and every track segment is sized at the cap; pages are committed only when written. A file whose estimated length exceeds the cap is refused before decoding; one that reaches the cap anyway stops there, flagged as truncated. The worker publishes `frames_ready` with release ordering, and the engine reads it with acquire ordering.
- **What exists:**
  - **Segments** (`specs/segments/spec.md`): `SegmentFactory` creates them, and `Region::map()` maps one, checking its header's magic number, layout version, and kind. `SegmentKind::Track` is 3. A 2 GiB segment uses memory only for the pages written, on Linux and on macOS (the lazy-memory criterion).
  - **The socket protocol** (`specs/socket-protocol/spec.md`): framed messages with descriptors, `decode()` and `encode()`, and `Connection`. Types 1 to 14 are taken.
  - **Logs** (`specs/logs/spec.md`): a process writes its lines to a log segment through a `LogWriter`.
  - **Processes** (`specs/processes/spec.md`): `ks-engine` is started with its descriptors inherited and named on its command line, takes its stop signals without handlers (`specs/audio-events/spec.md` requirement 9), and answers messages as requirement 7 there says. `ks::supervisor::spawn()` starts a program with given descriptors.
  - **The engine's `Track`** (`<ks/engine/track.h>`) holds a decoded track in private memory, with `kMinTrackRate`, `kMaxTrackRate`, and `kMaxTrackFrames`, and a `ready` word: the frame count in the low 63 bits, and "complete" in the top bit. Step 5b makes it read a track segment.
- **The API exists before the code:** the acceptance tests compile against the public headers. Until the maintainer approves this spec and the tests, the function bodies are stubs, and every acceptance test fails.

## Requirements

### FFmpeg

1. **The dependency:** `vcpkg.json` adds `ffmpeg` without its default features, with only `avcodec`, `avformat`, and `swresample`. It's built with FFmpeg's own decoders, so it decodes WAV, AIFF, FLAC, ALAC, MP3, AAC, Vorbis, and Opus without other libraries. Only `ks::decode` (requirement 9) links it.
2. **One thread:** FFmpeg decodes on the calling thread (`thread_count` 1), so decoding is deterministic and starts no threads of its own.

### The track segment

3. **`<ks/ipc/track_segment.h>`,** in `ks::ipc`, which the worker and, in step 5b, the engine and supervisor share:

   | Name | Value | Meaning |
   |---|---|---|
   | `kTrackLayoutVersion` | 1 | The layout version in a track segment's header |
   | `kMinTrackRate`, `kMaxTrackRate` | 8,000 and 384,000 | The sample rates a track can have, in Hz |
   | `kMaxTrackFrames` | 2²⁸ | The most frames a track can hold: 2 GiB of interleaved stereo floats |
   | `kTrackSamplesOffset` | 16,384 | Where the samples start: the largest page size on Linux and macOS, so they start on a page |
   | `kTrackSegmentSize` | `kTrackSamplesOffset` + 2³¹ | Every track segment's size, in bytes |
   | `kTrackComplete` | 2⁶³ | The "complete" bit in `ready` |

   `ks::engine`'s `kMinTrackRate`, `kMaxTrackRate`, and `kMaxTrackFrames` become these.
4. **`TrackHeader`,** at `kSegmentHeaderSize`, standard-layout and trivially copyable. The worker is its only writer, and writes each field through `store_shared()`:

   | Field | Type | Meaning |
   |---|---|---|
   | `sample_rate` | `std::uint64_t` | The track's rate in Hz. Written before the first store to `ready`, and not changed after |
   | `status` | `std::uint64_t` | A `TrackStatus`. Written before `ready` gets `kTrackComplete` |
   | `ready` | `std::uint64_t` | Frames decoded, in the low 63 bits, and `kTrackComplete` once no more will come. Stored with release ordering, only ever growing |

   `TrackStatus` is `Decoding` (0), `Complete` (1: the file ended), `Truncated` (2: decoding stopped at its limit), or `Failed` (3: an error or a stop ended decoding; the frames before it are good). A zero-filled header is a track not started yet.

   The samples start at `kTrackSamplesOffset`: interleaved stereo 32-bit floats, frame `n` at `kTrackSamplesOffset + 8n`. The worker writes a frame before publishing it, and never changes a frame once it's published.
5. **Reading the header:** `read_track(const TrackHeader&, std::uint64_t capacity)` reads it once, as another process may be writing it: `ready` with acquire ordering first, then the rest, relaxed. `capacity` is the frames the caller's own mapping holds. It returns a `TrackProgress` (`sample_rate`, `status`, `frames`, `complete`), with `frames` clamped to `capacity`, or a `TrackHeaderError`:
   - `BadRate`: `frames` is above 0, or the track is complete, and `sample_rate` is outside `kMinTrackRate` to `kMaxTrackRate`;
   - `BadStatus`: `status` isn't a `TrackStatus`; or the track is complete and its status is `Decoding`.

   A track that isn't complete reads as `Decoding`, whatever its status: the worker stores the final status just before it sets `kTrackComplete`, so a reader can find one without the other for a moment. A header with no frames, not complete, and status `Decoding` is valid whatever its `sample_rate`: the worker hasn't started.
6. **Writing:** `TrackWriter`, in the same header, writes a track segment that the caller mapped read-write:
   - `TrackWriter(std::span<std::byte> segment)`: the whole mapping, at least `kTrackSamplesOffset` bytes. Its capacity is `(segment.size() - kTrackSamplesOffset) / 8` frames, at most `kMaxTrackFrames`.
   - `start(std::uint32_t sample_rate)`: stores the rate.
   - `samples()`: the sample area, as `std::span<float>`, two floats per frame of capacity.
   - `publish(std::uint64_t frames)`: stores `ready`, with release ordering. `frames` never goes down and never exceeds the capacity.
   - `finish(TrackStatus status, std::uint64_t frames)`: stores the status, then `frames | kTrackComplete` in `ready`, with release ordering.
7. **The fuzz target** `ks_track_segment_fuzz` builds a `TrackHeader` and a capacity from its input and calls `read_track()`. It must never crash or trip a sanitizer, and every `TrackProgress` it returns must satisfy requirement 5: `frames` at most the capacity, a rate in range when `frames` is above 0 or the track is complete, and a status that agrees with `complete`. It runs 1,000,000 inputs under the `fuzz` preset.

### Messages

8. **Four new types,** and one new error code, following `specs/socket-protocol/spec.md` requirements 20 and 21:

   | Type | Number | From, to | Body | Descriptors | Request ID |
   |---|---|---|---|---|---|
   | `Probe` | 15 | Supervisor to worker | Empty | 1: the file, open for reading | Non-zero |
   | `Probed` | 16 | Worker to supervisor | `sample_rate`: u32; `channels`: u32; `estimated_frames`: u64 | 0 | The `Probe`'s |
   | `Decode` | 17 | Supervisor to worker | `max_frames`: u64 | 1: the track segment | Non-zero |
   | `Decoded` | 18 | Worker to supervisor | `frames`: u64; `status`: u8 | 0 | The `Decode`'s |

   - `Probed`: the file's sample rate and channel count, and its length in frames as FFmpeg estimates it before decoding, or 0 if it can't.
   - `Decode`: decode at most `max_frames` frames.
   - `Decoded`: how many frames were decoded, and how decoding ended: a `TrackStatus` other than `Decoding`.

   `decode()` refuses, with `BadField`:
   - a `Probed` whose `sample_rate` is outside `kMinTrackRate` to `kMaxTrackRate`, whose `channels` is 0 or above 64, or whose `estimated_frames` is above `kMaxTrackFrames`;
   - a `Decode` whose `max_frames` is 0 or above `kMaxTrackFrames`;
   - a `Decoded` whose `frames` is above `kMaxTrackFrames`, or whose `status` is `Decoding` or not a `TrackStatus`.

   `ErrorCode::Refused` (4): the sender won't do what was asked, for the reason in the text. The worker refuses this way (requirement 18).

### Decoding

9. **`ks::decode`,** a library in `src/decode/`, with its public header `<ks/decode/decoder.h>`. It links FFmpeg privately, and its header includes nothing from FFmpeg.
10. **The source:** a `Decoder` reads through a `Source`, an interface with `read_at(std::uint64_t offset, std::span<std::byte> buffer)` (the bytes read, fewer only at the end, 0 past it; or an `errno` value) and `size()` (the size in bytes, or an `errno` value). The decoder keeps its own position. `FdSource` reads a descriptor with `pread()` and sizes it with `fstat()`, so the descriptor's own offset doesn't matter; a descriptor that isn't a regular file answers both with `EINVAL`.
11. **Opening:** `Decoder::open(std::unique_ptr<Source>)` reads enough of the file to find its best audio stream (FFmpeg's `av_find_best_stream()`), and returns a `Decoder` or a `Refusal`, with a reason and a one-line text. It refuses a file:
    - `Unreadable`: FFmpeg can't open it as a file it knows, or the source fails;
    - `NoAudio`: it has no audio stream;
    - `Unsupported`: there's no decoder for its audio codec, or the decoder won't open;
    - `BadRate`: its sample rate is outside `kMinTrackRate` to `kMaxTrackRate`;
    - `BadChannels`: it has no channels, or more than 64;
    - `TooLong`: its estimated length is above `kMaxTrackFrames`. The text gives the length, and the cap at that rate, each as minutes and seconds (`m:ss`).
12. **What it reports:** `info()` gives the stream's sample rate, channel count, estimated frames, and codec name. The estimate is FFmpeg's duration for the stream, or else for the file, at the stream's rate, rounded to the nearest frame; it's 0 if FFmpeg gives neither.
13. **Decoding:** `read(std::span<float> out)` decodes into `out`, interleaved stereo, and returns the frames written: up to `out.size() / 2`, and fewer only at the end of the file. It returns 0 at the end, and keeps returning 0. Frames a call has no room for are kept for the next call, so the frames don't depend on the sizes of `out`.
    - **Format:** 32-bit float at the stream's own rate, never resampled. Integer samples are scaled exactly: a 16-bit sample `v` becomes `v / 32768`.
    - **Channels:** a stereo stream's channels pass through unchanged. A mono stream's samples go to both channels unchanged. Other layouts are mixed to stereo by swresample with its default matrix.
    - **Undecodable packets** are skipped and counted (`skipped_packets()`): the frames around them still decode.
    - **Errors:** if the source fails, or FFmpeg reports an error that isn't the end of the file, `read()` returns a `DecodeError` with a one-line text. Every frame returned before it is good. After an error, `read()` keeps returning the same error.
14. **`decode_track(Decoder&, TrackWriter&, std::uint64_t max_frames, const std::function<bool()>& stopping)`** decodes into a track segment, and returns the frames and status it finished with:
    1. it calls `start()` with the stream's rate;
    2. it decodes into the segment from frame 0, with at most 4,096 frames from each `read()`, and publishes after each `read()` that returns frames;
    3. it stops when the file ends (`Complete`), when it has written `max_frames` frames or the writer's capacity, whichever is less (`Truncated`), when `read()` returns an error (`Failed`), or when `stopping()` returns true, which it asks before each `read()` (`Failed`);
    4. it calls `finish()` with that status and frame count. A track that hits its limit exactly as the file ends is `Complete`.

    It writes nothing past the frames it publishes.

### The worker process

15. **`ks-worker`** lives in `src/apps/worker/`: a library, `ks::worker_app`, and a thin `main.cpp`. It depends on `ks::decode` and `ks::ipc`, and is built into `bin/` next to `ks-engine`. The rules of `specs/processes/spec.md` requirement 4 apply: every descriptor it creates is close-on-exec, and system calls interrupted by a signal are retried.
16. **Command line:** `ks-worker --supervisor-fd <n> [--log-fd <n>]`, or `--help`, which prints the usage and exits with status 0. A missing `--supervisor-fd`, and an unknown or repeated option, are usage errors: one line to standard error, starting `ks-worker: `, and exit status 2.
17. **Starting,** in this order. If a step fails, the worker writes one line to standard error and exits with status 1:
    1. it blocks `SIGTERM`, `SIGINT`, and `SIGHUP`, as `ks-engine` does, and installs no handlers;
    2. it checks that `--supervisor-fd` is an open stream socket, and `--log-fd`, if given, an open descriptor, and makes both close-on-exec;
    3. with `--log-fd`, it maps the log segment and starts a `LogWriter` on it, as `ks-engine` does (`specs/logs/spec.md` requirement 17).
18. **The job,** one per process. It answers messages as `specs/processes/spec.md` requirement 7 says: `Ping` with `Pong`, and a valid message it doesn't take at that point with `Error` `NotSupported`.
    1. **It waits for a `Probe`.** It opens a `Decoder` on an `FdSource` for the file. If the decoder refuses, it replies `Error` `Refused`, with the refusal's text, and exits with status 0. Otherwise it replies `Probed`.
    2. **It waits for a `Decode`.** It maps the segment read-write as kind `Track` at `kTrackLayoutVersion`. If that fails, or the mapping holds fewer than `max_frames` frames, it replies `Error` `Refused`, saying why, and exits with status 0. Otherwise it runs `decode_track()` with `max_frames`, replies `Decoded` with what it returns, and exits with status 0.
19. **Stopping:**
    - **A stop signal** (`SIGTERM`, `SIGINT`, or `SIGHUP`) stops it within 100 ms: while decoding, `stopping()` returns true, so the track finishes as `Failed`. It exits with status 0 without replying.
    - **The supervisor's socket closing** (end of file, or an error) before a `Decode` ends the job: it exits with status 0. While decoding, the worker finishes the track and exits without replying, so a track the engine is playing still gets every frame if the supervisor crashes (ADR 0003 §12).
    - **A frame the connection refuses or that doesn't decode** is answered as requirement 7 says, and closes the connection, which then counts as the socket closing.

    It reads the socket and checks for a signal at least every 10 ms while it waits, and between `read()` calls while it decodes.
20. **Its lines:** with `--log-fd`, every report goes to the log, and nothing goes to standard error except starting failures, which go to both. Without it, reports go to standard error. The levels:
    - **Info:** the probe's result: codec, rate, channels, and estimated length; and the track finishing `Complete`: frames, length in seconds, and how long decoding took;
    - **Warning:** a refusal, with its text; a track finishing `Truncated`; packets skipped, once, with the count; a frame refused from the supervisor; and a stop signal or the socket closing during the job;
    - **Error:** a track finishing `Failed` because of a decoding error, with its text.

    These lines contain these phrases: `refused: ` before a refusal's text; `decoded ` before a `Complete` track's frame count; `truncated at ` before a `Truncated` track's frame count; `failed after ` before a `Failed` track's frame count.

### The ADR

21. **ADR 0003 §6's steps for loading a track** become:
    1. A client asks the supervisor, giving the file's path.
    2. The supervisor opens the file and starts a worker, giving it the open file. The worker reports the file's sample rate and estimated length, or why it can't decode it.
    3. If the track fits the per-track cap and the memory budget (section 9), the supervisor creates a track segment and passes it to the worker and to the engine's control thread at the same time.
    4. The worker decodes into the segment.
    5. Meanwhile, the control thread maps the segment, locks it, and builds a track object. It hands the object to the audio thread through an internal ring (section 10).
    6. Playback can start before decoding finishes (section 9).

## Acceptance criteria

The fixtures are in `tests/acceptance/decode-worker/data/`, made by `make.sh` there with the `ffmpeg` command-line tool: 1-second, 1 kHz sine tones at −6 dBFS, stereo at 44.1 kHz unless named otherwise. Tests write their own WAV files where they need exact sample values or a long track.

The track segment (`track_segment_test.cpp`):

- [ ] `[auto]` **The layout:** `TrackHeader`'s fields are at offsets 0, 8, and 16; it's standard-layout and trivially copyable; `kTrackSegmentSize` is `kTrackSamplesOffset` + 2³¹; and `ks::engine`'s three constants equal `ks::ipc`'s. (test: "Track segment: the layout")
- [ ] `[auto]` **Reading:** `read_track()` accepts a zero-filled header as not started; accepts each status with `complete` as requirement 5 allows; clamps `frames` to the capacity; reads an incomplete track whose status is already final as `Decoding`; and refuses each case of requirement 5 with its error: a rate of 0, 7,999, and 384,001 with frames ready, a status of 4, and a complete track still `Decoding`. (test: "Track segment: reading the header")
- [ ] `[auto]` **Writing:** on a segment from `SegmentFactory`, `TrackWriter` stores what `read_track()` then reads back: the rate after `start()`, the frames after each `publish()`, and the status and frames after `finish()`, with `complete`. (test: "Track segment: writing")
- [ ] `[auto]` **Fuzzed:** `ks_track_segment_fuzz` runs 1,000,000 inputs clean under the `fuzz` preset. (test: the fuzz target)

Messages (`messages_test.cpp`):

- [ ] `[auto]` **The new types:** each of `Probe`, `Probed`, `Decode`, and `Decoded` encodes and decodes back to itself, with its descriptor count and request ID; `decode()` refuses each field value requirement 8 names, with `BadField`; and an `Error` with `Refused` round-trips. (test: "Messages: probing and decoding")
- [ ] `[auto]` **Fuzzed:** the socket-protocol fuzz target decodes the new types without further changes, and still runs 1,000,000 inputs clean under the `fuzz` preset. (test: `ks_socket_protocol_fuzz`)

Decoding (`decoder_test.cpp`):

- [ ] `[auto]` **Exact samples:** a 16-bit stereo WAV that the test writes, with every frame different, decodes to exactly its frames, each sample `v / 32768`. A mono one decodes with both channels equal to that. (test: "Decoder: 16-bit samples are exact")
- [ ] `[auto]` **Formats:** WAV (16-bit, and 32-bit float), AIFF, FLAC, ALAC, MP3, AAC, Vorbis, and Opus (at 48 kHz) each report their rate and 2 channels, and decode to a 1 kHz tone, measured within 0.5 Hz over the middle half second on each channel, at an RMS level within 0.5 dB of the source's. The lossless ones decode to exactly 44,100 frames, and the lossy ones to within 2,048 frames of 44,100 (or of 48,000 for Opus). (test: "Decoder: every format")
- [ ] `[auto]` **Not resampled:** a 96 kHz FLAC reports 96,000 Hz and decodes to 96,000 frames. (test: "Decoder: the file's own rate")
- [ ] `[auto]` **Downmixing:** a 5.1 FLAC with the tone only in the front left for a second, then only in the front right, then only in the centre, decodes to the tone in the left channel only (the right below −80 dBFS), then in the right only, then in both, equal sample for sample. (test: "Decoder: more than two channels")
- [ ] `[auto]` **Cover art:** an MP3 with a picture attached decodes like one without. (test: "Decoder: a file with a picture")
- [ ] `[auto]` **Refusals:** a text file is refused as `Unreadable`, an empty file as `Unreadable`, a pipe as `Unreadable`, a 4 kHz WAV as `BadRate`, and a FLAC whose header claims 2³⁰ frames as `TooLong`, with `405:48` (its length) in the text. (test: "Decoder: refused files")
- [ ] `[auto]` **Reported length:** `info()` estimates 44,100 frames for the 1-second FLAC and for the 16-bit WAV, and the codec names are `flac` and `pcm_s16le`. (test: "Decoder: the estimated length")
- [ ] `[auto]` **Read sizes don't matter:** the MP3 and the 5.1 FLAC decode to the same frames, bit for bit, whether `read()` is given room for 1, 37, 1,000, or 4,096 frames at a time. (test: "Decoder: the frames don't depend on read sizes")
- [ ] `[auto]` **A damaged file:** an MP3 with 2 KiB of its middle overwritten with pseudo-random bytes from a fixed seed decodes past the damage: the tone is there in its first and last 0.3 s, and it decodes to at least 80% of its frames. (test: "Decoder: damage in the middle")
- [ ] `[auto]` **A failing source:** a source that returns `EIO` after half the bytes of a WAV makes `read()` return a `DecodeError` after some frames, every one of which matches the same file decoded from an intact source, and then the same error again. (test: "Decoder: the source fails")

Decoding into a segment (`decode_track_test.cpp`):

- [ ] `[auto]` **Complete:** the FLAC decodes into a segment from `SegmentFactory` as `Complete` with 44,100 frames, the same as `read()` gives, with the header's rate 44,100, and every byte after the last frame zero. (test: "Decode track: a whole file")
- [ ] `[auto]` **Truncated:** with `max_frames` of 10,000, it's `Truncated` with 10,000 frames, and nothing after them is written. With `max_frames` of exactly 44,100, it's `Complete`. (test: "Decode track: the limit")
- [ ] `[auto]` **Failed:** with the failing source, it's `Failed`, with the frames decoded before the error. With `stopping()` true from its third call, it's `Failed` after two reads. (test: "Decode track: failures and stops")

The worker process (`worker_test.cpp`), with the test as its supervisor:

- [ ] `[auto]` **A job:** given the FLAC, the worker replies `Probed` with 44,100 Hz, 2 channels, and 44,100 frames; given a segment, it replies `Decoded` with 44,100 frames and `Complete`; the segment's frames equal the decoder's in the test's own process, bit for bit; and it exits with status 0. (test: "Worker: probing and decoding a file")
- [ ] `[auto]` **Progress:** decoding a 10-minute WAV that the test writes, the test reads `ready` as fast as it can and sees at least 10 different frame counts before the track is complete, never going down; and each time, the last frame published already holds its final values. (test: "Worker: frames are published as they're decoded")
- [ ] `[auto]` **Refusals:** a text file gets `Error` `Refused` with the decoder's refusal text, and the worker exits with status 0. A segment of the wrong kind, and one with too little room for `max_frames`, each get `Error` `Refused`, and the worker exits with status 0. (test: "Worker: refusing a file and a segment")
- [ ] `[auto]` **Messages out of turn:** `Ping` gets `Pong` at each stage; a `Decode` before a `Probe`, and a second `Probe`, get `Error` `NotSupported`, and the job carries on; a frame that doesn't decode gets `Error` `BadMessage` and the worker exits. (test: "Worker: messages out of turn")
- [ ] `[auto]` **Stopping:**
  - while it waits for a `Probe`, `SIGTERM` makes the worker exit with status 0 within 100 ms; closing the socket after `Probed` makes it exit with status 0;
  - during a decode of the 10-minute WAV, the test pauses the worker with `SIGSTOP` once frames are ready, and checks the track isn't complete yet (if it is, the test starts again, up to 5 times). Then `SIGTERM` and `SIGCONT` make it exit with status 0 within 100 ms, leaving the track complete as `Failed`, with fewer frames than the file and every one of them right. Closing the socket and `SIGCONT` instead let the decode finish: the track is `Complete`, with every frame.

  (test: "Worker: stopping")
- [ ] `[auto]` **Logging:** with a log segment, the worker's lines for a decoded track, a refused file, and a truncated track contain their phrases from requirement 20 at their levels, and nothing goes to standard error. (test: "Worker: its log")
- [ ] `[auto]` **Usage:** an unknown option and a missing `--supervisor-fd` exit with status 2 and a line starting `ks-worker: `; `--help` exits with status 0. (test: "Worker: usage")

Everywhere:

- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux in every preset, and on macOS arm64 in `debug`, `release`, `asan`, and `tsan`.

## Out of scope

- **Step 5b:**
  - a client's load request, and the supervisor opening the file, starting workers (at most 4 at once), creating segments, and passing them to the engine;
  - the memory budget, and what an unknown estimated length counts as;
  - the engine mapping a track segment read-only, building a `Track` on it, and releasing it;
  - the worker's log file, and what the supervisor does when a worker dies (the track ends where it got to, as `Failed`).
- **Step 7:** restarting a decode after a worker crash.
- **Step 10:** locking track memory (`mlock`), with the health checks.
- **Metadata:** tags, cover art, ReplayGain (Phase 2).
- **Resampling,** gapless trimming beyond what FFmpeg does by default, and seeking within a file: the worker decodes each file once, from the start.
- **Sandboxing** the worker (roadmap "Later").

## Open questions

None. The maintainer decided:

1. **Step 5 is split:** 5a, this spec, is the worker on its own; 5b loads tracks through the supervisor into the engine.
2. **One worker process per track,** which exits when the track is decoded (requirement 18).
3. **Track memory is locked in step 10,** with the health checks.
4. **A worker that dies or fails partway** leaves the track ending where it got to, as `Failed` (requirements 4 and 14). Step 5b makes the supervisor finish the track when a worker dies without finishing it.

These follow from them, and from the ADRs:

5. **The supervisor passes an open file, not a path** (requirement 8). The worker never resolves paths, so a later sandbox can deny it the file system; and a file deleted during decoding still decodes.
6. **The worker probes before it gets a segment** (requirements 8 and 18). The supervisor can then refuse a load that would exceed the memory budget before creating anything. That's a change to ADR 0003 §6 (requirement 21).
7. **The worker keeps decoding if the supervisor's socket closes** (requirement 19), as ADR 0003 §12 has the other processes keep running when the supervisor crashes.
8. **Samples start at 16 KiB** (requirement 3), on a page boundary on both platforms, so step 10 can lock pages as they fill.

Amended during the implementation, with the maintainer's approval:

9. **An incomplete track reads as `Decoding`** (requirement 5). The first version refused an incomplete header with a final status, but the worker writes exactly that for a moment before it sets `kTrackComplete`, and the `tsan` run of "Worker: frames are published as they're decoded" caught a reader refusing it.
10. **One earlier protected test changes:** `tests/acceptance/client-rings/messages_test.cpp` refused error code 4 as unknown, and 4 is now `Refused` (requirement 8), so that case uses 5. The client-rings step made the same change for code 3.
