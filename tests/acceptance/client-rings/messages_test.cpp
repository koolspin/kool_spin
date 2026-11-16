// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/client-rings/spec.md: the ring messages and the
// new error code (requirements 3 and 4), and the ring segment (requirement 1).

#include <ks/ipc/messages.h>

#include <ks/ipc/command.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/frame.h>
#include <ks/ipc/region.h>
#include <ks/ipc/segment.h>
#include <ks/ipc/spsc_ring.h>

#include "protocol_support.h"
#include "segments_support.h"
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using ks::ipc::decode;
using ks::ipc::encode;
using ks::ipc::ErrorCode;
using ks::ipc::Frame;
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

/// A slot's body, written out by hand: 4 bytes, little-endian.
std::vector<std::byte> slot_body(std::uint32_t slot) {
    return bytes({static_cast<int>(slot & 0xFFU), static_cast<int>((slot >> 8U) & 0xFFU),
                  static_cast<int>((slot >> 16U) & 0xFFU), static_cast<int>(slot >> 24U)});
}

/// An Error's body, written out by hand: the code in 2 bytes, then empty
/// text.
std::vector<std::byte> error_body(int code) {
    return bytes({code & 0xFF, code >> 8, 0, 0, 0, 0});
}

} // namespace

TEST_CASE("Ring messages: round trip", "[messages]") {
    const std::vector<Message> messages{
        {.request_id = 1, .body = msg::OpenRing{}},
        {.request_id = 2, .body = msg::Detached{}},
        {.request_id = 3, .body = msg::RingOpened{.slot = 0}},
        {.request_id = 3, .body = msg::RingOpened{.slot = 7}},
        {.request_id = 4, .body = msg::Attach{}},
        {.request_id = 4, .body = msg::Attached{.slot = 0}},
        {.request_id = 4, .body = msg::Attached{.slot = 7}},
        {.request_id = 5, .body = msg::Detach{.slot = 0}},
        {.request_id = 5, .body = msg::Detach{.slot = 7}},
        {.request_id = 6,
         .body =
             msg::Error{.code = ErrorCode::Unavailable, .text = "all 8 client slots are in use"}},
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

    // The numbers, descriptor counts, and bodies, as requirements 3 and 4
    // lay them out.
    CHECK(static_cast<int>(MessageType::OpenRing) == 9);
    CHECK(static_cast<int>(MessageType::RingOpened) == 10);
    CHECK(static_cast<int>(MessageType::Attach) == 11);
    CHECK(static_cast<int>(MessageType::Attached) == 12);
    CHECK(static_cast<int>(MessageType::Detach) == 13);
    CHECK(static_cast<int>(MessageType::Detached) == 14);
    CHECK(static_cast<int>(ErrorCode::Unavailable) == 3);
    CHECK(encode(Message{.request_id = 1, .body = msg::OpenRing{}}) ==
          frame(MessageType::OpenRing, 1, {}));
    CHECK(encode(Message{.request_id = 2, .body = msg::RingOpened{.slot = 7}}) ==
          frame(MessageType::RingOpened, 2, bytes({7, 0, 0, 0}), 1));
    CHECK(encode(Message{.request_id = 3, .body = msg::Attach{}}) ==
          frame(MessageType::Attach, 3, {}, 1));
    CHECK(encode(Message{.request_id = 4, .body = msg::Attached{.slot = 5}}) ==
          frame(MessageType::Attached, 4, bytes({5, 0, 0, 0})));
    CHECK(encode(Message{.request_id = 5, .body = msg::Detach{.slot = 6}}) ==
          frame(MessageType::Detach, 5, bytes({6, 0, 0, 0})));
    CHECK(encode(Message{.request_id = 6, .body = msg::Detached{}}) ==
          frame(MessageType::Detached, 6, {}));
    CHECK(encode(Message{.request_id = 7,
                         .body = msg::Error{.code = ErrorCode::Unavailable, .text = ""}}) ==
          frame(MessageType::Error, 7, error_body(3)));
}

TEST_CASE("Ring messages: what's refused", "[messages]") {
    struct Case {
        std::string name;
        Frame frame;
        MessageError error;
    };
    std::vector<Case> cases;
    // Slots out of range.
    for (const MessageType type :
         {MessageType::RingOpened, MessageType::Attached, MessageType::Detach}) {
        const std::uint8_t fds = type == MessageType::RingOpened ? 1 : 0;
        for (const std::uint32_t slot : {std::uint32_t{8}, std::uint32_t{0xFFFF'FFFF}}) {
            cases.push_back(
                {.name = std::format("type {} with slot {}", static_cast<int>(type), slot),
                 .frame = frame(type, 1, slot_body(slot), fds),
                 .error = MessageError::BadField});
        }
        // Bodies of the wrong length.
        cases.push_back({.name = std::format("type {} with a 3-byte body", static_cast<int>(type)),
                         .frame = frame(type, 1, bytes({0, 0, 0}), fds),
                         .error = MessageError::ShortBody});
        cases.push_back({.name = std::format("type {} with a 5-byte body", static_cast<int>(type)),
                         .frame = frame(type, 1, bytes({0, 0, 0, 0, 0}), fds),
                         .error = MessageError::LongBody});
    }
    const std::vector<Case> more{
        // Descriptor counts.
        {.name = "a RingOpened with no descriptor",
         .frame = frame(MessageType::RingOpened, 1, slot_body(0), 0),
         .error = MessageError::WrongFdCount},
        {.name = "a RingOpened with 2 descriptors",
         .frame = frame(MessageType::RingOpened, 1, slot_body(0), 2),
         .error = MessageError::WrongFdCount},
        {.name = "an Attach with no descriptor",
         .frame = frame(MessageType::Attach, 1, {}, 0),
         .error = MessageError::WrongFdCount},
        {.name = "an Attach with 2 descriptors",
         .frame = frame(MessageType::Attach, 1, {}, 2),
         .error = MessageError::WrongFdCount},
        {.name = "an OpenRing with a descriptor",
         .frame = frame(MessageType::OpenRing, 1, {}, 1),
         .error = MessageError::WrongFdCount},
        {.name = "an Attached with a descriptor",
         .frame = frame(MessageType::Attached, 1, slot_body(0), 1),
         .error = MessageError::WrongFdCount},
        // Request IDs.
        {.name = "an OpenRing with request ID 0",
         .frame = frame(MessageType::OpenRing, 0, {}),
         .error = MessageError::WrongRequestId},
        {.name = "an Attach with request ID 0",
         .frame = frame(MessageType::Attach, 0, {}, 1),
         .error = MessageError::WrongRequestId},
        {.name = "a Detach with request ID 0",
         .frame = frame(MessageType::Detach, 0, slot_body(0)),
         .error = MessageError::WrongRequestId},
        {.name = "a RingOpened with request ID 0",
         .frame = frame(MessageType::RingOpened, 0, slot_body(0), 1),
         .error = MessageError::WrongRequestId},
        {.name = "an Attached with request ID 0",
         .frame = frame(MessageType::Attached, 0, slot_body(0)),
         .error = MessageError::WrongRequestId},
        {.name = "a Detached with request ID 0",
         .frame = frame(MessageType::Detached, 0, {}),
         .error = MessageError::WrongRequestId},
        // Bodies that should be empty.
        {.name = "an OpenRing with a 1-byte body",
         .frame = frame(MessageType::OpenRing, 1, bytes({0})),
         .error = MessageError::LongBody},
        {.name = "an Attach with a 1-byte body",
         .frame = frame(MessageType::Attach, 1, bytes({0}), 1),
         .error = MessageError::LongBody},
        {.name = "a Detached with a 1-byte body",
         .frame = frame(MessageType::Detached, 1, bytes({0})),
         .error = MessageError::LongBody},
        // The first error code after Unavailable.
        {.name = "error code 4",
         .frame = frame(MessageType::Error, 1, error_body(4)),
         .error = MessageError::BadField},
    };
    cases.insert(cases.end(), more.begin(), more.end());
    for (const Case& c : cases) {
        CAPTURE(c.name);
        const std::expected<Message, MessageError> decoded = decode(c.frame);
        REQUIRE(!decoded.has_value());
        CHECK(decoded.error() == c.error);
    }

    // encode() makes the same checks.
    for (const Message& bad : {
             Message{.request_id = 1, .body = msg::RingOpened{.slot = 8}},
             Message{.request_id = 1, .body = msg::Attached{.slot = 8}},
             Message{.request_id = 1, .body = msg::Detach{.slot = 8}},
         }) {
        const std::expected<Frame, MessageError> refused = encode(bad);
        REQUIRE(!refused.has_value());
        CHECK(refused.error() == MessageError::BadField);
    }
    const auto open_without_id = encode(Message{.request_id = 0, .body = msg::OpenRing{}});
    REQUIRE(!open_without_id.has_value());
    CHECK(open_without_id.error() == MessageError::WrongRequestId);
}

TEST_CASE("Ring segments: layout", "[segments]") {
    STATIC_CHECK(ks::ipc::kCommandRingSegmentSize ==
                 ks::ipc::kSegmentHeaderSize + sizeof(ks::ipc::CommandRing));
    STATIC_CHECK(ks::ipc::kCommandRingLayoutVersion == 1);

    const ks::ipc::test::TempDir dir;
    REQUIRE(!dir.path().empty());
    std::expected<ks::ipc::SegmentFactory, std::error_code> factory =
        ks::ipc::SegmentFactory::open(dir.path());
    REQUIRE(factory.has_value());
    const std::expected<ks::ipc::Fd, std::error_code> segment =
        factory->create(ks::ipc::SegmentKind::CommandRing, ks::ipc::kCommandRingLayoutVersion,
                        ks::ipc::kCommandRingSegmentSize);
    REQUIRE(segment.has_value());

    const std::expected<ks::ipc::Region, ks::ipc::MapFailure> region =
        ks::ipc::Region::map(segment->get(), ks::ipc::SegmentKind::CommandRing,
                             ks::ipc::kCommandRingLayoutVersion, ks::ipc::Access::ReadWrite);
    REQUIRE(region.has_value());
    auto* const ring = region->at<ks::ipc::CommandRing>(ks::ipc::kSegmentHeaderSize);
    REQUIRE(ring != nullptr);
    ks::ipc::RingConsumer<ks::ipc::CommandSlot, ks::ipc::kCommandRingCapacity> consumer{*ring};
    CHECK_FALSE(consumer.try_pop().has_value());
    CHECK_FALSE(consumer.corrupt());
}
