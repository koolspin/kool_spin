// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The socket protocol's message types (specs/socket-protocol/spec.md
/// requirements 20 to 24).
///
/// Every type has a number that's never reused or renumbered (0 is never a
/// type), a body with fixed fields in a fixed order, an exact descriptor
/// count, and a rule for its request ID: non-zero for a request, the
/// request's ID for a reply, or 0 for a notification.
///
/// Bodies: integers are little-endian at their stated width, with no padding.
/// Text is a 32-bit byte length, then that many bytes of UTF-8 with no NUL,
/// overlong encodings, surrogates, or anything above U+10FFFF; each text field
/// has a maximum length. No field is optional, and a body must be used
/// exactly.
///
/// | Type    | Number | Body                          | Descriptors | Request ID          |
/// |---------|--------|-------------------------------|-------------|---------------------|
/// | Ping    | 1      | Empty                         | 0           | Non-zero            |
/// | Pong    | 2      | Empty                         | 0           | The Ping's          |
/// | Error   | 3      | Code (16 bits); text (≤ 1024) | 0           | The request's, or 0 |
/// | Hello   | 4      | Name: text (1 to 64)          | 0           | Non-zero            |
/// | Welcome | 5      | Empty                         | 1           | The Hello's         |
/// | Stop    | 6      | Empty                         | 0           | Non-zero            |
/// | Done    | 7      | Empty                         | 0           | The Stop's          |
/// | Ready   | 8      | Empty                         | 0           | 0                   |
/// | OpenRing   | 9   | Empty                         | 0           | Non-zero            |
/// | RingOpened | 10  | Slot (32 bits)                | 1           | The OpenRing's      |
/// | Attach     | 11  | Empty                         | 1           | Non-zero            |
/// | Attached   | 12  | Slot (32 bits)                | 0           | The Attach's        |
/// | Detach     | 13  | Slot (32 bits)                | 0           | Non-zero            |
/// | Detached   | 14  | Empty                         | 0           | The Detach's        |
///
/// Types 4 to 8 come from specs/processes/spec.md requirement 6, and types 9
/// to 14 from specs/client-rings/spec.md requirement 3. A slot is a client
/// slot's index, below kStateClients. Later steps add types with the same
/// rules, each in its own spec.

#include <ks/ipc/frame.h>
#include <ks/ipc/state.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <variant>

namespace ks::ipc {

/// The message types. The values are part of the protocol: never renumber or
/// reuse them. A frame carries the type in 16 bits.
enum class MessageType : std::uint8_t {
    Ping = 1,        ///< Asks the peer to answer with a Pong.
    Pong = 2,        ///< Answers a Ping.
    Error = 3,       ///< Reports that something the peer sent was refused.
    Hello = 4,       ///< A client introduces itself to the supervisor.
    Welcome = 5,     ///< The supervisor answers a Hello with the engine-state segment.
    Stop = 6,        ///< Asks the receiver to fade out and stop.
    Done = 7,        ///< Answers a Stop, once the receiver has stopped.
    Ready = 8,       ///< The engine is playing. A notification, with request ID 0.
    OpenRing = 9,    ///< A client asks the supervisor for a command ring.
    RingOpened = 10, ///< The supervisor answers an OpenRing with the ring segment.
    Attach = 11,     ///< The supervisor asks the engine to attach a ring segment.
    Attached = 12,   ///< The engine answers an Attach with the ring's client slot.
    Detach = 13,     ///< The supervisor asks the engine to detach a client slot's ring.
    Detached = 14,   ///< The engine answers a Detach once the slot is free again.
};

/// What an Error message reports. The values are part of the protocol. The
/// message carries the code in 16 bits.
enum class ErrorCode : std::uint8_t {
    /// The sender refused something the receiver sent, and is about to close
    /// the connection.
    BadMessage = 1,
    /// The sender doesn't accept that message from this peer, and carries on.
    NotSupported = 2,
    /// The sender takes that request from this peer, but can't do it now, and
    /// carries on. The peer may ask again later (specs/client-rings/spec.md
    /// requirement 4).
    Unavailable = 3,
};

/// The longest text an Error message carries, in bytes.
inline constexpr std::size_t kMaxErrorTextSize = 1024;

/// The longest name a Hello message carries, in bytes. A name has at least
/// one byte.
inline constexpr std::size_t kMaxClientNameSize = 64;

/// The bodies of the message types.
namespace msg {

struct Ping {
    friend bool operator==(const Ping&, const Ping&) = default;
};

struct Pong {
    friend bool operator==(const Pong&, const Pong&) = default;
};

struct Error {
    ErrorCode code = ErrorCode::BadMessage;
    std::string text; ///< UTF-8, up to kMaxErrorTextSize bytes.

    friend bool operator==(const Error&, const Error&) = default;
};

/// A client's first message to the supervisor.
struct Hello {
    std::string name; ///< UTF-8, 1 to kMaxClientNameSize bytes, for reports.

    friend bool operator==(const Hello&, const Hello&) = default;
};

/// The supervisor's answer to a Hello. Its one descriptor is the
/// engine-state segment, which the client maps read-only.
struct Welcome {
    friend bool operator==(const Welcome&, const Welcome&) = default;
};

/// Asks the receiver to fade out and stop: a client asks the supervisor, and
/// the supervisor asks the engine.
struct Stop {
    friend bool operator==(const Stop&, const Stop&) = default;
};

/// Answers a Stop, once the receiver has stopped.
struct Done {
    friend bool operator==(const Done&, const Done&) = default;
};

/// The engine tells the supervisor that it's playing.
struct Ready {
    friend bool operator==(const Ready&, const Ready&) = default;
};

/// A client asks the supervisor for a command ring.
struct OpenRing {
    friend bool operator==(const OpenRing&, const OpenRing&) = default;
};

/// The supervisor's answer to an OpenRing. Its one descriptor is the
/// command-ring segment, which the client maps read-write.
struct RingOpened {
    std::uint32_t slot = 0; ///< The ring's client slot, below kStateClients.

    friend bool operator==(const RingOpened&, const RingOpened&) = default;
};

/// The supervisor asks the engine to attach a ring. Its one descriptor is the
/// command-ring segment.
struct Attach {
    friend bool operator==(const Attach&, const Attach&) = default;
};

/// The engine's answer to an Attach.
struct Attached {
    std::uint32_t slot = 0; ///< The ring's client slot, below kStateClients.

    friend bool operator==(const Attached&, const Attached&) = default;
};

/// The supervisor asks the engine to detach a client slot's ring.
struct Detach {
    std::uint32_t slot = 0; ///< Below kStateClients.

    friend bool operator==(const Detach&, const Detach&) = default;
};

/// The engine's answer to a Detach, once the slot is free again.
struct Detached {
    friend bool operator==(const Detached&, const Detached&) = default;
};

} // namespace msg

/// A message: its request ID and its body.
struct Message {
    std::uint32_t request_id = 0;
    std::variant<msg::Ping, msg::Pong, msg::Error, msg::Hello, msg::Welcome, msg::Stop, msg::Done,
                 msg::Ready, msg::OpenRing, msg::RingOpened, msg::Attach, msg::Attached,
                 msg::Detach, msg::Detached>
        body;

    friend bool operator==(const Message&, const Message&) = default;
};

/// Why a frame isn't a valid message (requirement 22).
enum class MessageError : std::uint8_t {
    UnknownType,    ///< Not a MessageType.
    ShortBody,      ///< The body ends before its last field does.
    LongBody,       ///< Bytes are left over after the last field.
    BadField,       ///< A field's value is invalid, such as text that isn't UTF-8.
    WrongFdCount,   ///< The type carries a different number of descriptors.
    WrongRequestId, ///< A request or reply with ID 0, or a notification with one.
};

/// The message in `frame`, or why it isn't one. A function of the frame's
/// type, request ID, descriptor count, and body alone.
[[nodiscard]] std::expected<Message, MessageError> decode(const Frame& frame);

/// The frame for `message`, or why it can't be sent (the same checks decode()
/// makes). The encoding is canonical: encode(decode(frame)) gives back
/// `frame`.
[[nodiscard]] std::expected<Frame, MessageError> encode(const Message& message);

} // namespace ks::ipc
