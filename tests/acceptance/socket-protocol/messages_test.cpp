// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/socket-protocol/spec.md: message types.

#include <ks/ipc/messages.h>

#include <ks/ipc/frame.h>

#include "protocol_checks.h"
#include "protocol_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ks::ipc::decode;
using ks::ipc::encode;
using ks::ipc::ErrorCode;
using ks::ipc::Frame;
using ks::ipc::kMaxErrorTextSize;
using ks::ipc::Message;
using ks::ipc::MessageError;
using ks::ipc::MessageType;
using ks::ipc::test::bytes;
namespace msg = ks::ipc::msg;

/// A frame with the given fields.
Frame frame(std::uint16_t type, std::uint32_t request_id, std::vector<std::byte> body,
            std::uint8_t descriptors = 0) {
    return Frame{.type = type,
                 .request_id = request_id,
                 .descriptor_count = descriptors,
                 .body = std::move(body)};
}

/// The body of an Error message with `code` and `text`, written out by hand.
std::vector<std::byte> error_body(std::uint16_t code, std::string_view text) {
    std::vector<std::byte> body = bytes({code & 0xFF, code >> 8U});
    const auto length = static_cast<std::uint32_t>(text.size());
    for (unsigned shift = 0; shift < 32; shift += 8) {
        body.push_back(static_cast<std::byte>((length >> shift) & 0xFFU));
    }
    for (const char c : text) {
        body.push_back(static_cast<std::byte>(c));
    }
    return body;
}

constexpr auto kPing = static_cast<std::uint16_t>(MessageType::Ping);
constexpr auto kPong = static_cast<std::uint16_t>(MessageType::Pong);
constexpr auto kError = static_cast<std::uint16_t>(MessageType::Error);

} // namespace

TEST_CASE("Messages: round trip", "[messages]") {
    const std::vector<Message> messages{
        {.request_id = 1, .body = msg::Ping{}},
        {.request_id = 0xFFFF'FFFF, .body = msg::Ping{}},
        {.request_id = 1, .body = msg::Pong{}},
        {.request_id = 0, .body = msg::Error{.code = ErrorCode::BadMessage, .text = ""}},
        {.request_id = 5,
         .body = msg::Error{.code = ErrorCode::NotSupported,
                            .text = std::string(kMaxErrorTextSize, 'x')}},
        // "café ♫ 𝄞": two-, three-, and four-byte sequences.
        {.request_id = 0,
         .body = msg::Error{.code = ErrorCode::BadMessage,
                            .text = "caf\xC3\xA9 \xE2\x99\xAB \xF0\x9D\x84\x9E"}},
    };
    std::vector<std::byte> stream;
    for (const Message& message : messages) {
        const std::expected<Frame, MessageError> encoded = encode(message);
        REQUIRE(encoded.has_value());
        REQUIRE(encoded->descriptor_count == 0);
        const std::expected<Message, MessageError> decoded = decode(*encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(*decoded == message);
        REQUIRE(encode(*decoded) == *encoded);

        const std::expected<std::vector<std::byte>, ks::ipc::FrameError> wire =
            ks::ipc::encode_frame(encoded->type, encoded->request_id, encoded->body, 0);
        REQUIRE(wire.has_value());
        stream.insert(stream.end(), wire->begin(), wire->end());
    }

    // The layouts in requirement 23.
    REQUIRE(encode(Message{.request_id = 3, .body = msg::Ping{}}) == frame(kPing, 3, {}));
    REQUIRE(encode(Message{.request_id = 3, .body = msg::Pong{}}) == frame(kPong, 3, {}));
    REQUIRE(encode(Message{.request_id = 9,
                           .body = msg::Error{.code = ErrorCode::NotSupported, .text = "hi"}}) ==
            frame(kError, 9, bytes({0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 'h', 'i'})));

    // The fuzz target's checks hold on these messages, however they're read.
    const std::array<std::uint8_t, 5> controls{0x00, 0x13, 0x40, 0x80, 0xC5};
    for (const std::uint8_t control : controls) {
        CAPTURE(control);
        REQUIRE(ks::ipc::test::stream_checks_hold(control, stream));
    }
    REQUIRE(ks::ipc::test::frame_checks_hold(bytes(
        {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 'x'})));
}

TEST_CASE("Messages: what's refused", "[messages]") {
    struct Case {
        const char* what;
        Frame frame;
        MessageError error;
    };
    const std::vector<Case> cases{
        // MessageType is held in 8 bits, so no type is ever numbered 256.
        {"type 256", frame(256, 1, {}), MessageError::UnknownType},
        {"type 65,535", frame(0xFFFF, 1, {}), MessageError::UnknownType},
        {"an Error one byte short",
         frame(kError, 0,
               [] {
                   std::vector<std::byte> body = error_body(1, "hi");
                   body.pop_back();
                   return body;
               }()),
         MessageError::ShortBody},
        {"an Error with only half a code", frame(kError, 0, bytes({0x01})),
         MessageError::ShortBody},
        {"a Ping one byte long", frame(kPing, 1, bytes({0x00})), MessageError::LongBody},
        {"an Error one byte long",
         frame(kError, 0,
               [] {
                   std::vector<std::byte> body = error_body(1, "hi");
                   body.push_back(std::byte{0});
                   return body;
               }()),
         MessageError::LongBody},
        {"text that runs past the body",
         frame(kError, 0, bytes({0x01, 0x00, 0x64, 0x00, 0x00, 0x00, 'h', 'i'})),
         MessageError::ShortBody},
        {"1025 bytes of text",
         frame(kError, 0, error_body(1, std::string(kMaxErrorTextSize + 1, 'x'))),
         MessageError::BadField},
        {"a stray continuation byte", frame(kError, 0, error_body(1, "a\x80z")),
         MessageError::BadField},
        {"an overlong encoding", frame(kError, 0, error_body(1, "\xC0\xAF")),
         MessageError::BadField},
        {"a surrogate", frame(kError, 0, error_body(1, "\xED\xA0\x80")), MessageError::BadField},
        {"a sequence cut short", frame(kError, 0, error_body(1, "\xE2\x82")),
         MessageError::BadField},
        {"a code point past U+10FFFF", frame(kError, 0, error_body(1, "\xF4\x90\x80\x80")),
         MessageError::BadField},
        {"a NUL", frame(kError, 0, error_body(1, std::string_view{"a\0b", 3})),
         MessageError::BadField},
        {"error code 0", frame(kError, 0, error_body(0, "")), MessageError::BadField},
        {"error code 65535", frame(kError, 0, error_body(65'535, "")), MessageError::BadField},
        {"a Ping with request ID 0", frame(kPing, 0, {}), MessageError::WrongRequestId},
        {"a Pong with request ID 0", frame(kPong, 0, {}), MessageError::WrongRequestId},
        {"a Ping with a descriptor", frame(kPing, 1, {}, 1), MessageError::WrongFdCount},
        {"an Error with a descriptor", frame(kError, 0, error_body(1, ""), 1),
         MessageError::WrongFdCount},
    };
    for (const Case& test : cases) {
        CAPTURE(test.what);
        const std::expected<Message, MessageError> decoded = decode(test.frame);
        REQUIRE_FALSE(decoded.has_value());
        REQUIRE(decoded.error() == test.error);
    }

    // encode() refuses what decode() would.
    const std::expected<Frame, MessageError> no_id = encode(Message{.body = msg::Ping{}});
    REQUIRE_FALSE(no_id.has_value());
    REQUIRE(no_id.error() == MessageError::WrongRequestId);
    for (const msg::Error& bad : {
             msg::Error{.code = ErrorCode::BadMessage,
                        .text = std::string(kMaxErrorTextSize + 1, 'x')},
             msg::Error{.code = ErrorCode::BadMessage, .text = "\xC0\xAF"},
             msg::Error{.code = static_cast<ErrorCode>(9), .text = ""},
         }) {
        const std::expected<Frame, MessageError> refused = encode(Message{.body = bad});
        REQUIRE_FALSE(refused.has_value());
        REQUIRE(refused.error() == MessageError::BadField);
    }
}
