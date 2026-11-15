// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The socket protocol's message types (specs/socket-protocol/spec.md
/// requirements 20 to 24).
///
/// Every type has a number that's never reused or renumbered (0 is never a
/// type), a body with fixed fields in a fixed order, an exact descriptor
/// count, and a rule for its request ID: non-zero for a request, the
/// request's ID for a reply, or 0.
///
/// Bodies: integers are little-endian at their stated width, with no padding.
/// Text is a 32-bit byte length, then that many bytes of UTF-8 with no NUL,
/// overlong encodings, surrogates, or anything above U+10FFFF; each text field
/// has a maximum length. No field is optional, and a body must be used
/// exactly.
///
/// | Type  | Number | Body                          | Descriptors | Request ID           |
/// |-------|--------|-------------------------------|-------------|----------------------|
/// | Ping  | 1      | Empty                         | 0           | Non-zero             |
/// | Pong  | 2      | Empty                         | 0           | The Ping's           |
/// | Error | 3      | Code (16 bits); text (≤ 1024) | 0           | The request's, or 0  |
///
/// Later steps add types with the same rules, each in its own spec.

#include <ks/ipc/frame.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <variant>

namespace ks::ipc {

/// The message types. The values are part of the protocol: never renumber or
/// reuse them. A frame carries the type in 16 bits.
enum class MessageType : std::uint8_t {
    Ping = 1,  ///< Asks the peer to answer with a Pong.
    Pong = 2,  ///< Answers a Ping.
    Error = 3, ///< Reports that something the peer sent was refused.
};

/// What an Error message reports. The values are part of the protocol. The
/// message carries the code in 16 bits.
enum class ErrorCode : std::uint8_t {
    /// The sender refused something the receiver sent, and is about to close
    /// the connection.
    BadMessage = 1,
    /// The sender doesn't accept that message from this peer, and carries on.
    NotSupported = 2,
};

/// The longest text an Error message carries, in bytes.
inline constexpr std::size_t kMaxErrorTextSize = 1024;

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

} // namespace msg

/// A message: its request ID and its body.
struct Message {
    std::uint32_t request_id = 0;
    std::variant<msg::Ping, msg::Pong, msg::Error> body;

    friend bool operator==(const Message&, const Message&) = default;
};

/// Why a frame isn't a valid message (requirement 22).
enum class MessageError : std::uint8_t {
    UnknownType,    ///< Not a MessageType.
    ShortBody,      ///< The body ends before its last field does.
    LongBody,       ///< Bytes are left over after the last field.
    BadField,       ///< A field's value is invalid, such as text that isn't UTF-8.
    WrongFdCount,   ///< The type carries a different number of descriptors.
    WrongRequestId, ///< A request with ID 0, or a reply without one.
};

/// The message in `frame`, or why it isn't one. A function of the frame's
/// type, request ID, descriptor count, and body alone.
[[nodiscard]] std::expected<Message, MessageError> decode(const Frame& frame);

/// The frame for `message`, or why it can't be sent (the same checks decode()
/// makes). The encoding is canonical: encode(decode(frame)) gives back
/// `frame`.
[[nodiscard]] std::expected<Frame, MessageError> encode(const Message& message);

} // namespace ks::ipc
