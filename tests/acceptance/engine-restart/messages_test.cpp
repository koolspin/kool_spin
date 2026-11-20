// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/engine-restart/spec.md: the messages it adds and
// changes (requirements 6 and 17).

#include <ks/ipc/messages.h>

#include <ks/ipc/frame.h>
#include <ks/ipc/state.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>
#include <vector>

namespace {

using ks::ipc::decode;
using ks::ipc::encode;
using ks::ipc::Frame;
using ks::ipc::kStateClients;
using ks::ipc::Message;
using ks::ipc::MessageError;
using ks::ipc::MessageType;
namespace msg = ks::ipc::msg;

Frame frame(MessageType type, std::uint32_t request_id, std::vector<std::byte> body,
            std::uint8_t descriptors = 0) {
    return Frame{.type = static_cast<std::uint16_t>(type),
                 .request_id = request_id,
                 .descriptor_count = descriptors,
                 .body = std::move(body)};
}

std::vector<std::byte> slot_body(std::uint32_t slot) {
    std::vector<std::byte> body;
    body.reserve(4);
    for (int i = 0; i < 4; ++i) {
        body.push_back(static_cast<std::byte>((slot >> (8 * i)) & 0xFFU));
    }
    return body;
}

} // namespace

TEST_CASE("Restart messages: round trip and refusals", "[messages]") {
    SECTION("the numbers") {
        CHECK(static_cast<int>(MessageType::Attach) == 11);
        CHECK(static_cast<int>(MessageType::OpenRecording) == 27);
        CHECK(static_cast<int>(MessageType::RecordingOpened) == 28);
    }

    SECTION("an Attach carries its slot, below kStateClients") {
        for (std::uint32_t slot = 0; slot < kStateClients; ++slot) {
            const Message message{.request_id = 9, .body = msg::Attach{.slot = slot}};
            const std::expected<Frame, MessageError> encoded = encode(message);
            REQUIRE(encoded.has_value());
            CHECK(*encoded == frame(MessageType::Attach, 9, slot_body(slot), 1));
            const std::expected<Message, MessageError> decoded = decode(*encoded);
            REQUIRE(decoded.has_value());
            CHECK(*decoded == message);
        }
        CHECK(encode(Message{.request_id = 9, .body = msg::Attach{.slot = kStateClients}}) ==
              std::unexpected{MessageError::BadField});
        CHECK(decode(frame(MessageType::Attach, 9, slot_body(kStateClients), 1)) ==
              std::unexpected{MessageError::BadField});
        CHECK(decode(frame(MessageType::Attach, 9, slot_body(0xFFFF'FFFFU), 1)) ==
              std::unexpected{MessageError::BadField});
        CHECK(decode(frame(MessageType::Attach, 9, {}, 1)) ==
              std::unexpected{MessageError::ShortBody});
        auto longer = slot_body(1);
        longer.push_back(std::byte{0});
        CHECK(decode(frame(MessageType::Attach, 9, longer, 1)) ==
              std::unexpected{MessageError::LongBody});
    }

    SECTION("OpenRecording and RecordingOpened") {
        const Message open{.request_id = 4, .body = msg::OpenRecording{}};
        CHECK(encode(open) == frame(MessageType::OpenRecording, 4, {}, 0));
        CHECK(decode(frame(MessageType::OpenRecording, 4, {}, 0)) == open);

        const Message opened{.request_id = 4, .body = msg::RecordingOpened{}};
        CHECK(encode(opened) == frame(MessageType::RecordingOpened, 4, {}, 1));
        CHECK(decode(frame(MessageType::RecordingOpened, 4, {}, 1)) == opened);

        // Request IDs, descriptors, and bodies.
        CHECK(decode(frame(MessageType::OpenRecording, 0, {}, 0)) ==
              std::unexpected{MessageError::WrongRequestId});
        CHECK(decode(frame(MessageType::OpenRecording, 4, {}, 1)) ==
              std::unexpected{MessageError::WrongFdCount});
        CHECK(decode(frame(MessageType::OpenRecording, 4, {std::byte{0}}, 0)) ==
              std::unexpected{MessageError::LongBody});
        CHECK(decode(frame(MessageType::RecordingOpened, 0, {}, 1)) ==
              std::unexpected{MessageError::WrongRequestId});
        CHECK(decode(frame(MessageType::RecordingOpened, 4, {}, 0)) ==
              std::unexpected{MessageError::WrongFdCount});
        CHECK(decode(frame(MessageType::RecordingOpened, 4, {}, 2)) ==
              std::unexpected{MessageError::WrongFdCount});
        CHECK(decode(frame(MessageType::RecordingOpened, 4, {std::byte{0}}, 1)) ==
              std::unexpected{MessageError::LongBody});
    }

    SECTION("the next type is unknown") {
        // The first number after the new types: not a MessageType.
        Frame unknown = frame(MessageType::Ping, 1, {});
        unknown.type = 29;
        CHECK(decode(unknown) == std::unexpected{MessageError::UnknownType});
    }
}
