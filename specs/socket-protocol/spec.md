# Feature: Socket protocol

- **Status:** Approved by the maintainer on 2026-11-15. Protected: build loops can't change it.
- **Roadmap:** Phase 1b, step 2
- **Built by:** interactive

## Goal

Give kool_spin's processes their control channel: length-prefixed binary messages over Unix stream sockets, with file descriptors attached. The parts:

- a hand-written frame parser, fuzzed, so nothing a peer sends can crash the supervisor;
- connections that work in the supervisor's `poll` loop and in blocking clients alike;
- the socket that external clients connect to, where each end checks the other's user ID;
- the rules for message types, and the first three: Ping, Pong, and Error.

## Context

- **ADR 0003 sections that apply:**
  - §6: the supervisor is the hub, and the engine's only socket is to it. Children inherit one end of a `socketpair()`. External clients connect to a socket in the runtime directory, and the supervisor checks their user ID. The wire format is `SOCK_STREAM`, with length-prefixed binary messages (version, type, length, body) up to a fixed maximum size, and a hand-written, fuzzed parser. The supervisor is single-threaded: one `poll` loop.
  - §8: the socket message parser is one of the fuzz targets.
- **What exists** (`ks::ipc`, from step 1):
  - `Fd`, and `socket_pair()`, whose sockets never raise `SIGPIPE`.
  - `send_some()` and `receive_some()`: one `sendmsg` and one `recvmsg`, at most 8 descriptors each.
  - The runtime directory, and lock files.
- **Descriptors and stream sockets:** a stream socket has no message boundaries. Descriptors arrive with the first bytes of the `sendmsg` they were attached to, and `recvmsg` returns whatever bytes are there. This protocol makes it unambiguous which message a descriptor belongs to (requirements 5 and 6).
- **Who uses this later:**
  - the supervisor (step 3), with non-blocking sockets in its `poll` loop;
  - the engine's control thread, the decode worker, and `ks-play`.
- **The message types that later steps are expected to add** (not part of this spec):
  - step 3: the handshake, and passing the engine and each client their segments;
  - step 4: each process's log segment;
  - step 5: loading tracks: a client's request, the worker's decode job, the engine's track, and the replies;
  - step 10: the health report.

  None of them needs more than 8 descriptors, or a body of more than a few KiB. The longest field is a file path: `PATH_MAX` is 4096 bytes on Linux and 1024 on macOS.
- **The API exists before the code:** the acceptance tests compile against the public headers. Until the maintainer approves this spec and the tests, the function bodies are stubs, and every acceptance test fails.

## Requirements

### Library

1. Everything here goes in `ks::ipc`, which keeps its rule: it depends only on `ks::core`, the standard library, and POSIX, so the supervisor can use it. New public headers: `<ks/ipc/frame.h>`, `<ks/ipc/connection.h>`, `<ks/ipc/listener.h>`, and `<ks/ipc/messages.h>`.
2. Requirements 2 to 4 of `specs/segments/spec.md` apply here too:
   - nothing here runs on the audio thread, so it may allocate and make system calls;
   - every descriptor it creates is close-on-exec;
   - system calls interrupted by a signal are retried, and failures are returned, never thrown.

### Frames

3. **A frame** is a 16-byte header followed by a body. The header's fields are little-endian, at fixed offsets:

   | Offset | Bytes | Field | Valid values |
   |---|---|---|---|
   | 0 | 2 | Version | `kProtocolVersion`, which is 1 |
   | 2 | 2 | Type | Not 0 |
   | 4 | 4 | Body length | 0 to `kMaxBodySize` (65,536) |
   | 8 | 4 | Request ID | Any (requirement 20) |
   | 12 | 1 | Descriptor count | 0 to `kMaxFdsPerMessage` (8) |
   | 13 | 3 | Reserved | Zero |

4. **Checking the header:** a frame is refused, with its own error for each, if its version, type, body length, descriptor count, or reserved bytes aren't valid. The header is checked as soon as its 16 bytes have arrived, before any of the body is read or allocated.
5. **Sending:** each `sendmsg` carries bytes of one frame only, and a frame's descriptors go with its first `sendmsg`.
6. **Receiving:** a reader never reads past the end of the frame it's reading. The descriptors that arrive while it reads a frame therefore belong to that frame, and their number must equal the header's descriptor count. If it doesn't, the frame is refused, and the descriptors are closed.
7. **Errors are final:** a refused frame, or end of file in the middle of a frame, ends the connection. Nothing tries to find the next frame boundary.
8. **One version:** every kool_spin program speaks `kProtocolVersion`. A frame of any other version is refused like any other bad frame. Changing the header or any message type's body means a new version.
9. **`FrameReader`** is a state machine with no I/O, so it can be fuzzed:
   - `next()` returns the buffer to read into next: the rest of the header, or the rest of the body. It's never empty while a frame is incomplete.
   - `commit(bytes, descriptors)` records that `bytes` bytes arrived in that buffer, with `descriptors` descriptors. Once a frame is complete, it returns the frame's header fields and body.
   - After refusing a frame, it offers no buffer, and refuses everything else.
10. **`encode_frame(type, request_id, body, descriptor_count)`** returns the frame's bytes. It refuses a type of 0, a body longer than `kMaxBodySize`, and more than `kMaxFdsPerMessage` descriptors.

### Connections

11. **`Connection`** owns a connected Unix stream socket, which may be blocking or non-blocking, with one `FrameReader` and a queue of outgoing frames.
12. **`send(type, request_id, body, fds)`** queues a frame, with duplicates of `fds` that are close-on-exec, and writes nothing. The caller can close its own descriptors as soon as `send()` returns. It refuses, without queuing anything:
    - a frame `encode_frame()` refuses;
    - a descriptor that can't be duplicated;
    - a frame that would take the queue past `kMaxQueuedBytes` (1 MiB) or `kMaxQueuedFds` (64 descriptors), with its own error. The supervisor drops a client that stops reading rather than queuing for it without limit.
13. **`flush()`** writes queued frames in order, following requirement 5, and closes each frame's duplicates once they're sent. It returns when the queue is empty, when a non-blocking socket is full, or on an error such as `EPIPE`. `has_queued()` says whether frames are still waiting, for `poll`.
14. **`receive()`** reads until a frame is complete, and returns it with its descriptors as `Fd`s:
    - on a non-blocking socket with no complete frame yet, it returns "nothing yet", keeping what it has read;
    - end of file between frames is `Closed`, and in the middle of a frame it's `Truncated`;
    - it reads at most one frame per call, and never past it (requirement 6);
    - after any error, it returns the same error on every later call.

### The supervisor's socket

15. **Its path is `<runtime dir>/supervisor.sock`.** A path longer than `sockaddr_un` holds (104 bytes on macOS, 108 on Linux, including the final NUL) is refused with `ENAMETOOLONG`.
16. **`Listener::open(runtime_dir)`:**
    - if a socket file is already at the path, it unlinks it: a supervisor that died left it there. Anything else at the path, such as a regular file or a symbolic link, is refused and left alone;
    - it binds, sets the file's mode to 0600, and listens, with a non-blocking, close-on-exec socket.

    The caller must hold the supervisor's lock file (step 3), so that no live supervisor owns a socket it replaces. Destroying the `Listener` closes the socket and unlinks the file.
17. **`Listener::accept(expected_uid)`** accepts one pending connection, and returns its socket: non-blocking, close-on-exec, and never raising `SIGPIPE`. With no connection pending, it returns "nothing pending". A peer whose user ID isn't `expected_uid` is closed at once and reported with `permission_denied`. The supervisor passes its own effective user ID.
18. **`connect_to_supervisor(runtime_dir, expected_uid)`** connects to the supervisor's socket and returns a blocking, close-on-exec socket that never raises `SIGPIPE`. If the process listening there doesn't have the user ID `expected_uid`, it closes the socket and returns `permission_denied`. That stops a client from talking to an impostor, such as a socket another user planted in `/tmp`.
19. **`peer_user_id(socket)`** returns the user ID of the process at the other end of a connection made through the supervisor's socket: `SO_PEERCRED` on Linux, `getpeereid` on macOS. Children started by the supervisor inherit their sockets, so their user ID needs no check.

### Messages

20. **Message types** are numbered in `MessageType`. A number is never reused or renumbered, and 0 is never a type. Each type specifies:
    - its body's fields, in order;
    - exactly how many descriptors it carries;
    - its request ID: non-zero for a request, the request's ID for a reply, or 0.
21. **Body encoding:**
    - integers are little-endian at their stated width, with no padding or alignment;
    - text is a 32-bit byte length, then that many bytes of UTF-8, with no NUL, no overlong encodings, no surrogates, and nothing above U+10FFFF. Each text field has a maximum length in bytes;
    - no field is optional, and a body must be used exactly: bytes left over are an error.
22. **`decode(frame)`** turns a frame's type, request ID, body, and descriptor count into a typed message, or refuses it with its own error for each of:
    - an unknown type;
    - a body that's too short, too long, or has an invalid field;
    - the wrong number of descriptors;
    - the wrong kind of request ID.

    It's a function of those values alone. **`encode(message)`** produces the frame, and the encoding is canonical: encoding a decoded frame gives back the same bytes.
23. **The first types:**

    | Type | Number | Body | Descriptors | Request ID |
    |---|---|---|---|---|
    | `Ping` | 1 | Empty | 0 | Non-zero |
    | `Pong` | 2 | Empty | 0 | The `Ping`'s |
    | `Error` | 3 | Code (16 bits); text, up to 1024 bytes | 0 | The request's it answers, or 0 |

    - Any process answers a `Ping` with a `Pong`. The supervisor will use them to check that the engine's control thread is responsive (step 7).
    - `Error` codes: 1, `BadMessage`: the sender refused something the receiver sent, and is about to close the connection; 2, `NotSupported`: the sender doesn't accept that message from this peer, and carries on. An unknown code is refused.
24. **Later types** follow requirements 20 and 21, and each comes with the spec of the step that needs it.

### Fuzzing

25. **The fuzz target** `ks_socket_protocol_fuzz` feeds its input through a `FrameReader`, in chunk sizes and with descriptor counts that the input chooses, and decodes every frame that's completed. It must never crash or trip a sanitizer, and every frame that decodes must encode back to the same bytes. It runs 1,000,000 inputs under the `fuzz` preset.

## Acceptance criteria

The tests are in `tests/acceptance/socket-protocol/`. Each test that needs a runtime directory uses its own, in a temporary directory, so tests can run in parallel.

Frames (`frame_test.cpp`):

- [ ] `[auto]` **Layout:** a frame with known fields encodes to the bytes written out in the test, at the offsets in requirement 3, and `FrameReader` reads them back. (test: "Frames: the header's layout")
- [ ] `[auto]` **Bad headers:** each of these is refused with its own error once 16 bytes have arrived, and the reader offers no buffer for the body: version 0 or 2, type 0, a body length of 65,537, 9 descriptors, and each reserved byte set. (test: "Frames: every bad header is refused")
- [ ] `[auto]` **Any split:** a stream of frames, including empty bodies and a 65,536-byte body, read one byte at a time and in random chunk sizes gives the same frames. `next()` never offers more than the rest of the current frame. (test: "Frames: read in any split")
- [ ] `[auto]` **Descriptor counts:** a frame that arrives with fewer or more descriptors than its header says is refused. (test: "Frames: descriptor counts must match")
- [ ] `[auto]` **Encoding limits:** `encode_frame()` refuses type 0, a body of 65,537 bytes, and 9 descriptors. (test: "Frames: what can't be encoded")

Connections (`connection_test.cpp`):

- [ ] `[auto]` **Descriptors stay with their frames:** frames with 2, 0, and 3 descriptors, sent back to back over a `socket_pair()`, arrive with their own descriptors. Each refers to the right file (`fstat` device and inode) and is close-on-exec, although the sender closed its own copies right after `send()`. (test: "Connections: descriptors stay with their frames")
- [ ] `[auto]` **Partial reads and writes:** over a non-blocking `socket_pair()` with small socket buffers, a frame with a 65,536-byte body and 3 descriptors, then 100 small frames, all arrive intact and in order when one thread alternates `flush()` and `receive()`. `has_queued()` is true while the socket is full. (test: "Connections: big frames through small buffers")
- [ ] `[auto]` **Mismatched descriptors:** with a raw `sendmsg`, a header that says 1 descriptor with none attached ends the connection. So does a header that says none with 2 attached, and then the process has no more open descriptors than before. (test: "Connections: a descriptor count that doesn't match")
- [ ] `[auto]` **Closing:** `receive()` returns `Closed` when the peer closes between frames, and `Truncated` when it closes in the middle of one, and the same error again on the next call. `flush()` to a closed peer returns `EPIPE`, and the process isn't killed. (test: "Connections: closed by the peer")
- [ ] `[auto]` **Bounded queue:** when the peer doesn't read, `send()` refuses a frame that would queue more than 1 MiB, or more than 64 descriptors, and queues nothing. Once the peer reads, every frame queued before arrives. (test: "Connections: the outgoing queue is bounded")

The supervisor's socket (`listener_test.cpp`):

- [ ] `[auto]` **Connecting:** two clients connect with `connect_to_supervisor()`, the listener accepts both, and each side sends a `Ping` and gets a `Pong` with the same request ID. The socket file has mode 0600, and it's gone once the `Listener` is destroyed. `accept()` with nothing pending returns "nothing pending". (test: "Supervisor socket: clients connect")
- [ ] `[auto]` **User IDs:** `accept()` with an `expected_uid` other than the process's own refuses the connection with `permission_denied`, and the client finds it closed. `connect_to_supervisor()` with another `expected_uid` refuses the listener. `peer_user_id()` gives the process's own user ID at both ends of an accepted connection. (test: "Supervisor socket: user IDs are checked")
- [ ] `[auto]` **The path:** a socket file left behind (bound, then closed without being unlinked) is replaced. A regular file, and a symbolic link to a socket, at the path are refused and left unchanged. A runtime directory whose socket path doesn't fit `sockaddr_un` is refused with `ENAMETOOLONG`. (test: "Supervisor socket: what's at the path")

Messages (`messages_test.cpp`):

- [ ] `[auto]` **Round trip:** `Ping`, `Pong`, and `Error` with each code and with text of 0 bytes, 1024 bytes, and multi-byte UTF-8 each encode and decode to the same message, and encode again to the same bytes. (test: "Messages: round trip")
- [ ] `[auto]` **Refused:** each of these is refused with the right error: types 4 and 65,535; a body one byte short and one byte long; a text length that runs past the body; 1025 bytes of text; invalid UTF-8 (a stray continuation byte, an overlong encoding, a surrogate, a sequence cut short, and a NUL); an unknown error code; a `Ping` with request ID 0; and a `Ping` with a descriptor. (test: "Messages: what's refused")

Fuzzing (`protocol_fuzz.cpp`):

- [ ] `[auto]` **Fuzzed:** the fuzz target in requirement 25 runs 1,000,000 inputs clean under the `fuzz` preset. (test: `ks_socket_protocol_fuzz`)

Everywhere:

- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux, in every preset, and on macOS arm64 in `debug`, `release`, `asan`, and `tsan`.

## Out of scope

- **Step 3:** the handshake (who sends what first, roles, client IDs), passing segments to the engine and clients, and the supervisor's `poll` loop and its policies, such as timeouts and when to drop a client.
- **Every other message type:** each comes with the step that needs it (requirement 24).
- **Talking across versions:** all kool_spin programs ship together, so a version mismatch is a bug, and the connection just closes.
- **Checking anything beyond the user ID,** such as which program a client is.
- **Windows.**

## Open questions

None. The maintainer approved these decisions:

1. **Only Ping, Pong, and Error now** (requirements 23 and 24). The Phase 1b plan said step 2 would list the message types. This spec defines only the types whose meaning doesn't depend on later steps, and the rules every type follows. The handshake, segments, and track loading come with steps 3 and 5, where their content is designed.
2. **Two header fields beyond ADR 0003's** (requirement 3): a request ID, so a client can match replies to requests while other messages arrive, and a descriptor count, so a frame's descriptors can be checked.
3. **One frame at a time** (requirements 5 and 6). Reading only up to the end of the current frame makes it exact which frame a descriptor belongs to. It costs at least two `recvmsg` calls per frame, which doesn't matter at the rate of control messages.
4. **The limits:** bodies of up to 64 KiB; up to 1 MiB and 64 descriptors queued per connection. After an engine restart, the supervisor passes it at most 8 tracks (one per deck slot), plus the rings and its other segments. macOS shells often allow only 256 open files, so a higher descriptor limit would be hard to test there.
5. **A version mismatch just closes the connection,** without saying why (requirement 8).
6. **Both ends check the user ID** (requirements 17 and 18). ADR 0003 asks only the supervisor to check its clients. Checking the supervisor from the client side also stops a client from trusting a socket planted by another user.
7. **The socket's path and stale files** (requirements 15 and 16): `supervisor.sock` in the runtime directory. A leftover socket file is replaced, which is safe only because the caller holds the supervisor's lock file.
