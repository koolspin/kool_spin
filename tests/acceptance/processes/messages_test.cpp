// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/processes/spec.md: the message types it adds
// (requirement 6).

#include <ks/ipc/messages.h>

#include <ks/ipc/frame.h>

#include "protocol_support.h"
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

namespace {

using ks::ipc::decode;
using ks::ipc::encode;
using ks::ipc::Frame;
using ks::ipc::kMaxClientNameSize;
using ks::ipc::Message;
using ks::ipc::MessageError;
using ks::ipc::MessageType;
using ks::ipc::test::bytes;
namespace msg = ks::ipc::msg;

Frame frame(MessageType type, std::uint32_t request_id, std::vector<std::byte> body,
            std::uint8_t descriptors = 0) {
    return Frame{.type = static_cast<std::uint16_t>(type),
                 .request_id = request_id,
                 .descriptor_count = descriptors,
                 .body = std::move(body)};
}

/// A Hello's body for `name`, written out by hand: a 32-bit length, then the
/// bytes.
std::vector<std::byte> name_body(const std::string& name) {
    const auto size = static_cast<std::uint32_t>(name.size());
    std::vector<std::byte> body =
        bytes({static_cast<int>(size & 0xFFU), static_cast<int>((size >> 8U) & 0xFFU),
               static_cast<int>((size >> 16U) & 0xFFU), static_cast<int>(size >> 24U)});
    for (const char c : name) {
        body.push_back(static_cast<std::byte>(c));
    }
    return body;
}

} // namespace

TEST_CASE("Process messages: round trip", "[messages]") {
    const std::vector<Message> messages{
        {.request_id = 1, .body = msg::Hello{"a"}},
        {.request_id = 2, .body = msg::Hello{std::string(kMaxClientNameSize, 'x')}},
        {.request_id = 3, .body = msg::Hello{"ks-play \xC3\xA9\xE2\x82\xAC\xF0\x9F\x8E\xA7"}},
        {.request_id = 4, .body = msg::Welcome{}},
        {.request_id = 5, .body = msg::Stop{}},
        {.request_id = 5, .body = msg::Done{}},
        {.request_id = 0, .body = msg::Ready{}},
    };
    for (std::size_t i = 0; i < messages.size(); ++i) {
        CAPTURE(i);
        const std::expected<Frame, MessageError> encoded = encode(messages[i]);
        REQUIRE(encoded.has_value());
        const std::expected<Message, MessageError> decoded = decode(*encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(*decoded == messages[i]);
        const std::expected<Frame, MessageError> again = encode(*decoded);
        REQUIRE(again.has_value());
        REQUIRE(*again == *encoded);
    }

    // The types, descriptor counts, and a body, as requirement 6 lays them out.
    const auto hello = encode(Message{.request_id = 1, .body = msg::Hello{"ab"}});
    REQUIRE(hello.has_value());
    CHECK(*hello == frame(MessageType::Hello, 1, bytes({2, 0, 0, 0, 'a', 'b'})));
    CHECK(static_cast<int>(MessageType::Hello) == 4);
    CHECK(static_cast<int>(MessageType::Welcome) == 5);
    CHECK(static_cast<int>(MessageType::Stop) == 6);
    CHECK(static_cast<int>(MessageType::Done) == 7);
    CHECK(static_cast<int>(MessageType::Ready) == 8);
    const auto welcome = encode(Message{.request_id = 4, .body = msg::Welcome{}});
    REQUIRE(welcome.has_value());
    CHECK(*welcome == frame(MessageType::Welcome, 4, {}, 1));
    const auto ready = encode(Message{.request_id = 0, .body = msg::Ready{}});
    REQUIRE(ready.has_value());
    CHECK(*ready == frame(MessageType::Ready, 0, {}));
}

TEST_CASE("Process messages: what's refused", "[messages]") {
    struct Case {
        const char* name;
        Frame frame;
        MessageError error;
    };
    const std::vector<Case> cases{
        {"a Hello with an empty name", frame(MessageType::Hello, 1, name_body("")),
         MessageError::BadField},
        {"a Hello with a 65-byte name",
         frame(MessageType::Hello, 1, name_body(std::string(kMaxClientNameSize + 1, 'x'))),
         MessageError::BadField},
        {"a Hello with invalid UTF-8", frame(MessageType::Hello, 1, name_body("a\xFF")),
         MessageError::BadField},
        {"a Welcome with no descriptor", frame(MessageType::Welcome, 1, {}, 0),
         MessageError::WrongFdCount},
        {"a Welcome with 2 descriptors", frame(MessageType::Welcome, 1, {}, 2),
         MessageError::WrongFdCount},
        {"a Hello with request ID 0", frame(MessageType::Hello, 0, name_body("a")),
         MessageError::WrongRequestId},
        {"a Stop with request ID 0", frame(MessageType::Stop, 0, {}), MessageError::WrongRequestId},
        {"a Done with request ID 0", frame(MessageType::Done, 0, {}), MessageError::WrongRequestId},
        {"a Ready with request ID 1", frame(MessageType::Ready, 1, {}),
         MessageError::WrongRequestId},
        {"a Welcome with a 1-byte body", frame(MessageType::Welcome, 1, bytes({0}), 1),
         MessageError::LongBody},
        {"a Stop with a 1-byte body", frame(MessageType::Stop, 1, bytes({0})),
         MessageError::LongBody},
        {"a Done with a 1-byte body", frame(MessageType::Done, 1, bytes({0})),
         MessageError::LongBody},
        {"a Ready with a 1-byte body", frame(MessageType::Ready, 0, bytes({0})),
         MessageError::LongBody},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        const std::expected<Message, MessageError> decoded = decode(c.frame);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error() == c.error);
    }

    // encode() makes the same checks.
    const auto empty_name = encode(Message{.request_id = 1, .body = msg::Hello{""}});
    REQUIRE_FALSE(empty_name.has_value());
    CHECK(empty_name.error() == MessageError::BadField);
    const auto long_name = encode(
        Message{.request_id = 1, .body = msg::Hello{std::string(kMaxClientNameSize + 1, 'x')}});
    REQUIRE_FALSE(long_name.has_value());
    CHECK(long_name.error() == MessageError::BadField);
    const auto ready_with_id = encode(Message{.request_id = 1, .body = msg::Ready{}});
    REQUIRE_FALSE(ready_with_id.has_value());
    CHECK(ready_with_id.error() == MessageError::WrongRequestId);
    const auto stop_without_id = encode(Message{.request_id = 0, .body = msg::Stop{}});
    REQUIRE_FALSE(stop_without_id.has_value());
    CHECK(stop_without_id.error() == MessageError::WrongRequestId);
}
