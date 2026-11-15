# Feature: Client command rings

- **Status:** Approved by the maintainer on 2026-11-15. Protected: build loops can't change it.
- **Roadmap:** Phase 1b, step 3b
- **Built by:** interactive

## Goal

Let a client control the engine. A client asks the supervisor for a command ring. The supervisor creates the ring's segment and gives it to the engine, which attaches it to a client slot, and then to the client. The client writes commands into the ring, and reads what the engine applied from its slot in the status block. When the client leaves, its ring is detached. Rings keep working if the supervisor dies.

## Context

- **ADRs that apply:**
  - **ADR 0003 §4:** one single-producer, single-consumer ring per client, in its own segment, with 64-byte slots. The engine reads each ring from the audio thread, and publishes the last sequence number it applied for each client.
  - **ADR 0003 §6:** the supervisor creates every segment and passes it on as a descriptor. Real-time data never passes through the supervisor: clients write rings. The supervisor keeps its own descriptor of a segment until the segment is no longer needed.
  - **ADR 0003 §7:** a command-ring segment is written by its client (slots and head) and by the engine (tail), and the engine maps it read-write.
  - **ADR 0003 §12:** when the supervisor dies, clients that already have rings keep control through them. The engine keeps its segments' descriptors, which leaves room for a new supervisor to adopt it later.
- **What exists:**
  - **`ks::ipc`:** `CommandSlot`, `CommandRing` (256 slots), `RingProducer`, and `RingConsumer`; `SegmentKind::CommandRing`; segments, the socket protocol, and the messages up to `Ready` (`specs/processes/spec.md` requirement 6).
  - **`ks::engine`:** `Engine::attach()` puts a ring in the lowest free client slot, of `kMaxClients` (8). `detach()` stops reading a slot's ring, and `collect()` returns the ring once the audio thread has let it go. A slot is in use from `attach()` until that release has been collected (`specs/commands/spec.md` requirement 12). The status block reports each slot as a `ClientAck`: attached, corrupt, commands read, the last sequence number read, and rejections (`specs/state-out/spec.md` requirement 5).
  - **`kool_spin` and `ks-engine`** (`specs/processes/spec.md`): clients say `Hello` and get the engine-state segment. Step 3b is the "commands" that requirement 18 there leaves to this step.
- **This spec extends** `specs/processes/spec.md`: requirement 10 (what the engine takes from the supervisor), requirement 18 (what the supervisor takes from a client), requirement 21 (what the supervisor takes from the engine), and requirement 22 (reports). The rest of that spec still applies unchanged.
- **The API exists before the code:** the acceptance tests compile against the public headers. Until the maintainer approves this spec and the tests, the function bodies are stubs, and every acceptance test fails.

## Requirements

### The ring segment

1. **Layout,** in `<ks/ipc/command.h>`:
   - `kCommandRingLayoutVersion` is 1;
   - `kCommandRingSegmentSize` is `kSegmentHeaderSize + sizeof(CommandRing)`;
   - a ring segment holds a `CommandRing` at offset `kSegmentHeaderSize`.

   The supervisor creates each ring segment with `SegmentFactory::create(SegmentKind::CommandRing, kCommandRingLayoutVersion, kCommandRingSegmentSize)`. A new segment is zero-filled apart from its header, so its ring is empty.
2. **Rules from the earlier steps** (`specs/segments/spec.md` requirements 2 to 4) apply: every descriptor is close-on-exec, and system calls interrupted by a signal are retried. Nothing new runs on the audio thread: it already reads attached rings.

### Messages

3. **Six new types,** following `specs/socket-protocol/spec.md` requirements 20 and 21:

   | Type | Number | From, to | Body | Descriptors | Request ID |
   |---|---|---|---|---|---|
   | `OpenRing` | 9 | Client to supervisor | Empty | 0 | Non-zero |
   | `RingOpened` | 10 | Supervisor to client | Slot: 32 bits | 1: the ring segment | The `OpenRing`'s |
   | `Attach` | 11 | Supervisor to engine | Empty | 1: the ring segment | Non-zero |
   | `Attached` | 12 | Engine to supervisor | Slot: 32 bits | 0 | The `Attach`'s |
   | `Detach` | 13 | Supervisor to engine | Slot: 32 bits | 0 | Non-zero |
   | `Detached` | 14 | Engine to supervisor | Empty | 0 | The `Detach`'s |

   A slot is a client slot's index. `decode()` refuses a slot of `kStateClients` (8) or more with `BadField`.
4. **A new error code:** 3, `Unavailable`. The sender takes that request from this peer, but can't do it now. It carries on, and the text says why. The peer may ask again later.

### The engine

5. **From the supervisor,** the engine takes `Ping`, `Stop`, `Attach`, and `Detach`. It answers anything else as `specs/processes/spec.md` requirement 7 says.
6. **`Attach`:** the control thread:
   1. maps the segment read-write, as kind `CommandRing` at `kCommandRingLayoutVersion`, and finds the ring at `kSegmentHeaderSize`;
   2. calls `Engine::attach()` with the ring;
   3. answers `Attached` with the slot that `attach()` returned.

   If the segment is refused, the ring doesn't fit in it, or `attach()` returns no slot, the engine unmaps the segment, closes its descriptor, and answers `Error` `Unavailable`. It keeps playing.

   While a ring is attached, the engine keeps both its mapping and its descriptor (ADR 0003 §12).
7. **`Detach`:** if the slot holds a ring that isn't already being detached, the control thread calls `Engine::detach()` for it. If the give ring is full, it tries again every control interval until it succeeds. Once `collect()` returns the ring's release, the engine unmaps the segment, closes its descriptor, and answers `Detached`. The slot is free from then on.

   A slot that holds no ring, or whose ring is already being detached, is answered with `Error` `Unavailable`.
8. **Without a supervisor:** rings stay attached, and the engine keeps reading them, so clients that have rings keep control (ADR 0003 §12). A detach that was under way still completes, without an answer.

### The supervisor

9. **From a client,** after `Hello`, the supervisor takes `Ping`, `Stop`, and `OpenRing`. As before, a client's first message must be `Hello`.
10. **`OpenRing`:**
    - **One ring per client:** a client that has a ring, or whose ring is being opened, is answered with `Error` `NotSupported`.
    - **While stopping:** answered with `Error` `NotSupported`, as `Hello` is.
    - **Otherwise,** the supervisor creates a ring segment and sends it to the engine in an `Attach`. It serves other clients while it waits for the answer. When the answer comes:
      - **`Attached`:** it answers the client with `RingOpened`, carrying the slot and the segment. The ring is now the client's;
      - **`Error`:** it closes its descriptor of the segment, and answers the client with `Error` `Unavailable`. The client may ask again.

      If the supervisor can't create the segment, it answers `Error` `Unavailable` at once.

    The supervisor doesn't count slots: the engine decides whether one is free. There's no time limit on the engine's answer. Step 7 deals with an engine that stops answering.
11. **A client leaving:** when a client closes its connection or is dropped:
    - if it has a ring, the supervisor sends the engine `Detach` with the ring's slot;
    - if its ring is being opened, the supervisor waits for the engine's answer. If that's `Attached`, it sends `Detach` for the slot.

    The supervisor keeps its own descriptor of a ring segment until the engine answers its `Detach`.
12. **From the engine,** the supervisor also takes `Attached`, `Detached`, and an `Error` that answers an `Attach` or a `Detach` it sent. It answers an `Attached` or `Detached` that answers no request it sent with `Error` `NotSupported`, as it answers any other message it doesn't take.
13. **Reports:** the supervisor also writes one line to standard error, starting `kool_spin: `, when:
    - a client's ring is opened, with the client's name and the slot;
    - a client's `OpenRing` is refused, and why;
    - a ring is detached, with its slot.

    As before, the wording isn't part of the interface.

### Clients

14. **Using a ring:** a client maps the segment from `RingOpened` read-write, as kind `CommandRing` at `kCommandRingLayoutVersion`. It writes commands through an `ipc::RingProducer`, and reads what the engine did with them from slot `slot` of the status block's `clients`. The ring keeps working while the engine runs, even if the supervisor's connection closes. Nothing a client writes into its ring affects the engine's other clients (`specs/commands/spec.md` requirement 9).

## Acceptance criteria

The tests are in `tests/acceptance/client-rings/`. They follow `specs/processes/spec.md`'s rules for tests: separate runtime directories, the null backend, and no process outliving its test. They use the helpers in `tests/acceptance/processes/`.

"Applied" below means: within 2 s, the mixer's state block shows the command's value, and the client's `ClientAck` has `kClientAttached` set and `last_sequence` equal to the command's sequence number.

Messages (`messages_test.cpp`):

- [ ] `[auto]` **Round trip:** each of these encodes and decodes to the same message, and encodes again to the same bytes:
  - `OpenRing`, `Detached`;
  - `RingOpened`, `Attached`, and `Detach`, each with slots 0 and 7. A slot's body is its 4 bytes, little-endian;
  - `Error` `Unavailable` with a text.

  (test: "Ring messages: round trip")
- [ ] `[auto]` **Refused:** each of these is refused with the right error:
  - `RingOpened`, `Attached`, and `Detach` with slot 8, and with slot 2³² − 1;
  - `RingOpened` and `Attach` with no descriptor, and with 2; `OpenRing` and `Attached` with 1;
  - `OpenRing`, `Attach`, and `Detach` with request ID 0; `RingOpened`, `Attached`, and `Detached` with request ID 0;
  - a 3-byte and a 5-byte body for each type with a slot; a 1-byte body for each type whose body is empty;
  - `Error` with code 4.

  (test: "Ring messages: what's refused")
- [ ] `[auto]` **The segment:** `kCommandRingSegmentSize` is `kSegmentHeaderSize + sizeof(CommandRing)`, and `kCommandRingLayoutVersion` is 1. A segment created as requirement 1 says maps read-write as a ring segment, and its ring is empty: a `RingConsumer` attached to it pops nothing. (test: "Ring segments: layout")

The engine process (`engine_rings_test.cpp`). In these tests, the test plays the supervisor, as in `specs/processes/spec.md`'s engine tests, and creates ring segments as requirement 1 says:

- [ ] `[auto]` **Attach, apply, detach:** an `Attach` is answered with `Attached`, slot 0. Through the ring, `SetCrossfader` 0.25 with sequence number 1 is applied, then `SetMainLevel` −6 dB with sequence number 2. The slot's `commands_read` is 2 and `rejected` is 0. A `Detach` for slot 0 is answered with `Detached`. Then slot 0's `kClientAttached` is clear, and a `SetCrossfader` 0.75 written after it isn't applied within 500 ms. (test: "Engine rings: attach, apply, and detach")
- [ ] `[auto]` **Eight at most:** eight `Attach`es are answered with `Attached`, slots 0 to 7 in order. A ninth is answered with `Error` `Unavailable`, with the `Attach`'s request ID. After a `Detach` for slot 3 is answered with `Detached`, another `Attach` is answered with `Attached`, slot 3. (test: "Engine rings: eight at most")
- [ ] `[auto]` **What it refuses:** each of these is answered with `Error` `Unavailable`, and the engine's heartbeat keeps increasing:
  - an `Attach` with an engine-state segment;
  - an `Attach` with a ring segment whose layout version is 2;
  - an `Attach` with a ring segment of one page, too small for the ring;
  - a `Detach` for slot 5, which holds no ring;
  - a second `Detach` for a slot, sent straight after the first, before its `Detached`.

  Afterwards a `Ping` is still answered, and a valid `Attach` is answered with `Attached`. (test: "Engine rings: what it refuses")
- [ ] `[auto]` **A corrupt ring:** two rings are attached. The test sets the first ring's head 1,000 past its tail. Within 2 s, the first slot's `ClientAck` has `kClientCorrupt` set, and a command through the second ring is applied. (test: "Engine rings: a corrupt ring affects only its client")
- [ ] `[auto]` **Without a supervisor:** with a ring attached, the test closes its end of the socket. A command through the ring is still applied. `SIGTERM` then ends the engine with exit status 0. (test: "Engine rings: work without the supervisor")

The supervisor (`supervisor_rings_test.cpp`). In these tests, the test spawns `kool_spin` with `ks-engine`, as in `specs/processes/spec.md`'s supervisor tests:

- [ ] `[auto]` **Opening a ring:** after `Hello`, an `OpenRing` is answered with `RingOpened`, slot 0, whose descriptor maps read-write as a ring segment. A command through the ring is applied. (test: "Supervisor rings: a client opens a ring")
- [ ] `[auto]` **What it refuses:**
  - a second `OpenRing` from a client that has a ring is answered with `Error` `NotSupported`, and its ring still works;
  - an `OpenRing` as a client's first message is answered with `Error` `BadMessage`, and the connection closes;
  - `Attach`, `Attached`, `Detach`, and `Detached` from a client are each answered with `Error` `NotSupported`.

  (test: "Supervisor rings: what it refuses")
- [ ] `[auto]` **Detached when the client leaves:** a client opens a ring in slot 0, then closes its connection. Within 2 s, slot 0's `kClientAttached` is clear. Another client that then asks, retrying on `Unavailable` for up to 2 s, gets slot 0. (test: "Supervisor rings: detached when the client leaves")
- [ ] `[auto]` **Eight at most, and none leaked:**
  - eight clients each open a ring, and their slots are 0 to 7;
  - a ninth client's `OpenRing` is answered with `Error` `Unavailable`, and it still gets a `Pong`;
  - after one of the eight closes its connection, the ninth, retrying for up to 2 s, gets that client's slot;
  - after all nine close, twenty more clients each send `Hello` and `OpenRing` and close at once, without reading an answer;
  - then eight new clients each get a ring, retrying for up to 2 s, in slots 0 to 7.

  (test: "Supervisor rings: eight at most, and none leaked")
- [ ] `[auto]` **Rings outlive the supervisor:** a client opens a ring. After `SIGKILL` to `kool_spin`, the client's connection closes, and a command through its ring is still applied. `SIGTERM` to the engine then ends it. (test: "Supervisor rings: rings outlive the supervisor")

Everywhere:

- [ ] `[auto]` **The earlier steps' tests** all still pass. One protected test changes: `tests/acceptance/socket-protocol/messages_test.cpp` refuses error code 3 as unknown, and 3 is now `Unavailable`, so that case uses 65,535 instead (decision 4).
- [ ] `[auto]` **Every CI configuration:** all of the above pass on Linux in every preset, and on macOS arm64 in `debug`, `release`, `asan`, and `tsan`.
- [ ] `[auto]` **Fuzzing:** the socket-protocol fuzz target, `ks_socket_protocol_fuzz`, decodes the new types without further changes, and still runs 1,000,000 inputs clean under the `fuzz` preset.

## Out of scope

- **A client library:** step 6 adds one with `ks-play`, its first user. The tests talk to the supervisor through `ks::ipc` directly.
- **More than one ring per client, and closing a ring without disconnecting.**
- **Step 5:** loading tracks.
- **Step 7:** giving a restarted engine the rings again, and a time limit on the engine's answers.
- **Step 8:** a new supervisor adopting a running engine and its rings.

## Open questions

None. The maintainer approved these decisions:

1. **Two hops, six types** (requirement 3). The client asks the supervisor (`OpenRing`, `RingOpened`), and the supervisor asks the engine (`Attach`, `Attached`, `Detach`, `Detached`). Each hop has its own types because each type carries an exact number of descriptors: the segment goes to the engine in the request, and to the client in the reply.
2. **The engine decides about slots** (requirements 7 and 10). It chooses the slot, and it's the only one that knows whether a slot is free, so the supervisor doesn't count them. `Detached` comes only once the slot is free again, so a detached slot can be reused straight away.
3. **One ring per client, detached when the client leaves** (requirements 10 and 11). A client that's done disconnects; there's no message to close a ring.
4. **A new error code, `Unavailable`** (requirement 4), for "valid, but not now": all slots in use, or a segment the engine refused. `NotSupported` stays "not from you". This changes one case in a protected socket-protocol test, which uses 3 as an unknown code. The criterion there says only "an unknown error code", so that spec doesn't change.
5. **The engine keeps each ring's descriptor** while it's attached, as it keeps the state segment's (requirement 6), which leaves room for adoption in a later step (ADR 0003 §12).
6. **No time limit on the engine's answers** (requirement 10) until step 7, which watches the engine and restarts it.
