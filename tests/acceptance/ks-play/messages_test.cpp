// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/ks-play/spec.md: the message types it adds
// (requirement 1).

#include <ks/ipc/messages.h>

#include <ks/ipc/frame.h>
#include <ks/ipc/track_segment.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <utility>
#include <vector>

namespace {

using ks::ipc::decode;
using ks::ipc::encode;
using ks::ipc::Frame;
using ks::ipc::kMaxTrackFrames;
using ks::ipc::Message;
using ks::ipc::MessageError;
using ks::ipc::MessageType;
using ks::ipc::TrackStatus;
namespace msg = ks::ipc::msg;

Frame frame(MessageType type, std::uint32_t request_id, std::vector<std::byte> body,
            std::uint8_t descriptors = 0) {
    return Frame{.type = static_cast<std::uint16_t>(type),
                 .request_id = request_id,
                 .descriptor_count = descriptors,
                 .body = std::move(body)};
}

/// Appends `value` to `body`, little-endian, in `size` bytes.
void append(std::vector<std::byte>& body, std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        body.push_back(static_cast<std::byte>(value & 0xFFU));
        value >>= 8U;
    }
}

/// An UnloadDeck's body, written out by hand.
std::vector<std::byte> unload_body(std::uint64_t deck) {
    std::vector<std::byte> body;
    append(body, deck, 4);
    return body;
}

/// A DecodeEnded's body, written out by hand: the track ID, the frames, then
/// the status in one byte.
std::vector<std::byte> ended_body(std::uint64_t track_id, std::uint64_t frames,
                                  std::uint64_t status) {
    std::vector<std::byte> body;
    append(body, track_id, 8);
    append(body, frames, 8);
    append(body, status, 1);
    return body;
}

std::vector<std::byte> shortened(std::vector<std::byte> body) {
    body.pop_back();
    return body;
}

std::vector<std::byte> lengthened(std::vector<std::byte> body) {
    body.push_back(std::byte{0});
    return body;
}

constexpr std::uint64_t kLargestId = std::numeric_limits<std::uint64_t>::max();

} // namespace

TEST_CASE("Play messages: round trip", "[messages]") {
    SECTION("the numbers") {
        CHECK(static_cast<int>(MessageType::UnloadDeck) == 24);
        CHECK(static_cast<int>(MessageType::DeckUnloaded) == 25);
        CHECK(static_cast<int>(MessageType::DecodeEnded) == 26);
    }

    SECTION("round trips") {
        const std::vector<Message> messages{
            {.request_id = 1, .body = msg::UnloadDeck{.deck = 0}},
            {.request_id = 2, .body = msg::UnloadDeck{.deck = 3}},
            {.request_id = 3, .body = msg::DeckUnloaded{}},
            {.request_id = 0,
             .body = msg::DecodeEnded{.track_id = 1, .frames = 0, .status = TrackStatus::Complete}},
            {.request_id = 0,
             .body = msg::DecodeEnded{.track_id = 9,
                                      .frames = kMaxTrackFrames,
                                      .status = TrackStatus::Truncated}},
            {.request_id = 0,
             .body = msg::DecodeEnded{.track_id = kLargestId,
                                      .frames = 20'480,
                                      .status = TrackStatus::Failed}},
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
    }

    SECTION("the frames, as requirement 1 lays them out") {
        const auto unload = encode(Message{.request_id = 5, .body = msg::UnloadDeck{.deck = 2}});
        REQUIRE(unload.has_value());
        CHECK(*unload == frame(MessageType::UnloadDeck, 5, unload_body(2)));

        const auto unloaded = encode(Message{.request_id = 5, .body = msg::DeckUnloaded{}});
        REQUIRE(unloaded.has_value());
        CHECK(*unloaded == frame(MessageType::DeckUnloaded, 5, {}));

        const auto ended = encode(Message{
            .request_id = 0,
            .body = msg::DecodeEnded{
                .track_id = 0x1'0000'0002, .frames = 44'100, .status = TrackStatus::Complete}});
        REQUIRE(ended.has_value());
        CHECK(*ended == frame(MessageType::DecodeEnded, 0, ended_body(0x1'0000'0002, 44'100, 1)));
    }
}

TEST_CASE("Play messages: what's refused", "[messages]") {
    struct Case {
        const char* name;
        Frame frame;
        MessageError error;
    };
    const std::vector<std::byte> unload = unload_body(0);
    const std::vector<std::byte> ended = ended_body(1, 100, 1);
    const std::vector<Case> cases{
        // Fields.
        {"an UnloadDeck for deck 4", frame(MessageType::UnloadDeck, 1, unload_body(4)),
         MessageError::BadField},
        {"a DecodeEnded for track 0", frame(MessageType::DecodeEnded, 0, ended_body(0, 100, 1)),
         MessageError::BadField},
        {"a DecodeEnded with more than the cap",
         frame(MessageType::DecodeEnded, 0, ended_body(1, kMaxTrackFrames + 1, 1)),
         MessageError::BadField},
        {"a DecodeEnded still decoding", frame(MessageType::DecodeEnded, 0, ended_body(1, 100, 0)),
         MessageError::BadField},
        {"a DecodeEnded with status 4", frame(MessageType::DecodeEnded, 0, ended_body(1, 100, 4)),
         MessageError::BadField},
        {"a DecodeEnded with status 200",
         frame(MessageType::DecodeEnded, 0, ended_body(1, 100, 200)), MessageError::BadField},
        // Descriptors.
        {"an UnloadDeck with a descriptor", frame(MessageType::UnloadDeck, 1, unload, 1),
         MessageError::WrongFdCount},
        {"a DeckUnloaded with a descriptor", frame(MessageType::DeckUnloaded, 1, {}, 1),
         MessageError::WrongFdCount},
        {"a DecodeEnded with a descriptor", frame(MessageType::DecodeEnded, 0, ended, 1),
         MessageError::WrongFdCount},
        // Request IDs.
        {"an UnloadDeck with request ID 0", frame(MessageType::UnloadDeck, 0, unload),
         MessageError::WrongRequestId},
        {"a DeckUnloaded with request ID 0", frame(MessageType::DeckUnloaded, 0, {}),
         MessageError::WrongRequestId},
        {"a DecodeEnded with request ID 1", frame(MessageType::DecodeEnded, 1, ended),
         MessageError::WrongRequestId},
        // Body lengths.
        {"a short UnloadDeck", frame(MessageType::UnloadDeck, 1, shortened(unload)),
         MessageError::ShortBody},
        {"a long UnloadDeck", frame(MessageType::UnloadDeck, 1, lengthened(unload)),
         MessageError::LongBody},
        {"a DeckUnloaded with a 1-byte body", frame(MessageType::DeckUnloaded, 1, {std::byte{0}}),
         MessageError::LongBody},
        {"a short DecodeEnded", frame(MessageType::DecodeEnded, 0, shortened(ended)),
         MessageError::ShortBody},
        {"a long DecodeEnded", frame(MessageType::DecodeEnded, 0, lengthened(ended)),
         MessageError::LongBody},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        const std::expected<Message, MessageError> decoded = decode(c.frame);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error() == c.error);
    }

    // encode() makes the same checks.
    struct Bad {
        const char* name;
        Message message;
        MessageError error;
    };
    const std::vector<Bad> bad{
        {"deck 4", {.request_id = 1, .body = msg::UnloadDeck{.deck = 4}}, MessageError::BadField},
        {"a DecodeEnded for track 0",
         {.request_id = 0,
          .body = msg::DecodeEnded{.track_id = 0, .frames = 1, .status = TrackStatus::Complete}},
         MessageError::BadField},
        {"a DecodeEnded with more than the cap",
         {.request_id = 0,
          .body = msg::DecodeEnded{.track_id = 1,
                                   .frames = kMaxTrackFrames + 1,
                                   .status = TrackStatus::Complete}},
         MessageError::BadField},
        {"a DecodeEnded still decoding",
         {.request_id = 0,
          .body = msg::DecodeEnded{.track_id = 1, .frames = 1, .status = TrackStatus::Decoding}},
         MessageError::BadField},
        {"a DecodeEnded with request ID 1",
         {.request_id = 1,
          .body = msg::DecodeEnded{.track_id = 1, .frames = 1, .status = TrackStatus::Failed}},
         MessageError::WrongRequestId},
        {"a DeckUnloaded with request ID 0",
         {.request_id = 0, .body = msg::DeckUnloaded{}},
         MessageError::WrongRequestId},
        {"an UnloadDeck with request ID 0",
         {.request_id = 0, .body = msg::UnloadDeck{.deck = 0}},
         MessageError::WrongRequestId},
    };
    for (const Bad& b : bad) {
        CAPTURE(b.name);
        const std::expected<Frame, MessageError> encoded = encode(b.message);
        REQUIRE_FALSE(encoded.has_value());
        CHECK(encoded.error() == b.error);
    }
}
