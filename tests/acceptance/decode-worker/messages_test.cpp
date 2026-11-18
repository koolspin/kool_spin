// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/decode-worker/spec.md: the message types and the
// error code it adds (requirement 8).

#include <ks/ipc/messages.h>

#include <ks/ipc/frame.h>
#include <ks/ipc/track_segment.h>

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
using ks::ipc::ErrorCode;
using ks::ipc::Frame;
using ks::ipc::kMaxTrackFrames;
using ks::ipc::kMaxTrackRate;
using ks::ipc::kMinTrackRate;
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

/// A Probed's body, written out by hand.
std::vector<std::byte> probed_body(std::uint64_t rate, std::uint64_t channels,
                                   std::uint64_t estimated) {
    std::vector<std::byte> body;
    append(body, rate, 4);
    append(body, channels, 4);
    append(body, estimated, 8);
    return body;
}

/// A Decode's body, written out by hand.
std::vector<std::byte> decode_body(std::uint64_t max_frames) {
    std::vector<std::byte> body;
    append(body, max_frames, 8);
    return body;
}

/// A Decoded's body, written out by hand.
std::vector<std::byte> decoded_body(std::uint64_t frames, std::uint64_t status) {
    std::vector<std::byte> body;
    append(body, frames, 8);
    append(body, status, 1);
    return body;
}

/// `body` without its last byte.
std::vector<std::byte> shortened(std::vector<std::byte> body) {
    body.pop_back();
    return body;
}

/// `body` with one more byte.
std::vector<std::byte> lengthened(std::vector<std::byte> body) {
    body.push_back(std::byte{0});
    return body;
}

} // namespace

TEST_CASE("Messages: probing and decoding", "[messages]") {
    SECTION("the numbers") {
        CHECK(static_cast<int>(MessageType::Probe) == 15);
        CHECK(static_cast<int>(MessageType::Probed) == 16);
        CHECK(static_cast<int>(MessageType::Decode) == 17);
        CHECK(static_cast<int>(MessageType::Decoded) == 18);
        CHECK(static_cast<int>(ErrorCode::Refused) == 4);
    }

    SECTION("round trips") {
        const std::vector<Message> messages{
            {.request_id = 1, .body = msg::Probe{}},
            {.request_id = 1,
             .body = msg::Probed{.sample_rate = 44'100, .channels = 2, .estimated_frames = 44'100}},
            {.request_id = 2,
             .body =
                 msg::Probed{.sample_rate = kMinTrackRate, .channels = 1, .estimated_frames = 0}},
            {.request_id = 3,
             .body = msg::Probed{.sample_rate = kMaxTrackRate,
                                 .channels = 64,
                                 .estimated_frames = kMaxTrackFrames}},
            {.request_id = 4, .body = msg::Decode{.max_frames = 1}},
            {.request_id = 5, .body = msg::Decode{.max_frames = kMaxTrackFrames}},
            {.request_id = 4, .body = msg::Decoded{.frames = 0, .status = TrackStatus::Complete}},
            {.request_id = 5,
             .body = msg::Decoded{.frames = kMaxTrackFrames, .status = TrackStatus::Truncated}},
            {.request_id = 6, .body = msg::Decoded{.frames = 1'234, .status = TrackStatus::Failed}},
            {.request_id = 7,
             .body = msg::Error{.code = ErrorCode::Refused,
                                .text = "refused: not an audio file it can read"}},
            {.request_id = 0, .body = msg::Error{.code = ErrorCode::Refused, .text = ""}},
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

    SECTION("the frames, as requirement 8 lays them out") {
        const auto probe = encode(Message{.request_id = 9, .body = msg::Probe{}});
        REQUIRE(probe.has_value());
        CHECK(*probe == frame(MessageType::Probe, 9, {}, 1));

        const auto probed =
            encode(Message{.request_id = 9,
                           .body = msg::Probed{
                               .sample_rate = 96'000, .channels = 6, .estimated_frames = 300'000}});
        REQUIRE(probed.has_value());
        CHECK(*probed == frame(MessageType::Probed, 9, probed_body(96'000, 6, 300'000)));

        const auto decode_frame =
            encode(Message{.request_id = 10, .body = msg::Decode{.max_frames = 44'100}});
        REQUIRE(decode_frame.has_value());
        CHECK(*decode_frame == frame(MessageType::Decode, 10, decode_body(44'100), 1));

        const auto decoded = encode(
            Message{.request_id = 10,
                    .body = msg::Decoded{.frames = 10'000, .status = TrackStatus::Truncated}});
        REQUIRE(decoded.has_value());
        CHECK(*decoded == frame(MessageType::Decoded, 10, decoded_body(10'000, 2)));
    }

    SECTION("what's refused") {
        struct Case {
            const char* name;
            Frame frame;
            MessageError error;
        };
        const std::vector<Case> cases{
            // Probed's fields.
            {"a Probed at 7,999 Hz", frame(MessageType::Probed, 1, probed_body(7'999, 2, 1)),
             MessageError::BadField},
            {"a Probed at 384,001 Hz", frame(MessageType::Probed, 1, probed_body(384'001, 2, 1)),
             MessageError::BadField},
            {"a Probed at 0 Hz", frame(MessageType::Probed, 1, probed_body(0, 2, 1)),
             MessageError::BadField},
            {"a Probed with no channels", frame(MessageType::Probed, 1, probed_body(44'100, 0, 1)),
             MessageError::BadField},
            {"a Probed with 65 channels", frame(MessageType::Probed, 1, probed_body(44'100, 65, 1)),
             MessageError::BadField},
            {"a Probed estimating more than the cap",
             frame(MessageType::Probed, 1, probed_body(44'100, 2, kMaxTrackFrames + 1)),
             MessageError::BadField},
            // Decode's field.
            {"a Decode of 0 frames", frame(MessageType::Decode, 1, decode_body(0), 1),
             MessageError::BadField},
            {"a Decode of more than the cap",
             frame(MessageType::Decode, 1, decode_body(kMaxTrackFrames + 1), 1),
             MessageError::BadField},
            // Decoded's fields.
            {"a Decoded of more than the cap",
             frame(MessageType::Decoded, 1, decoded_body(kMaxTrackFrames + 1, 1)),
             MessageError::BadField},
            {"a Decoded still Decoding", frame(MessageType::Decoded, 1, decoded_body(5, 0)),
             MessageError::BadField},
            {"a Decoded with status 4", frame(MessageType::Decoded, 1, decoded_body(5, 4)),
             MessageError::BadField},
            {"a Decoded with status 255", frame(MessageType::Decoded, 1, decoded_body(5, 255)),
             MessageError::BadField},
            // Descriptors.
            {"a Probe with no descriptor", frame(MessageType::Probe, 1, {}, 0),
             MessageError::WrongFdCount},
            {"a Probe with 2 descriptors", frame(MessageType::Probe, 1, {}, 2),
             MessageError::WrongFdCount},
            {"a Decode with no descriptor", frame(MessageType::Decode, 1, decode_body(5), 0),
             MessageError::WrongFdCount},
            {"a Probed with a descriptor",
             frame(MessageType::Probed, 1, probed_body(44'100, 2, 1), 1),
             MessageError::WrongFdCount},
            {"a Decoded with a descriptor", frame(MessageType::Decoded, 1, decoded_body(5, 1), 1),
             MessageError::WrongFdCount},
            // Request IDs.
            {"a Probe with request ID 0", frame(MessageType::Probe, 0, {}, 1),
             MessageError::WrongRequestId},
            {"a Probed with request ID 0", frame(MessageType::Probed, 0, probed_body(44'100, 2, 1)),
             MessageError::WrongRequestId},
            {"a Decode with request ID 0", frame(MessageType::Decode, 0, decode_body(5), 1),
             MessageError::WrongRequestId},
            {"a Decoded with request ID 0", frame(MessageType::Decoded, 0, decoded_body(5, 1)),
             MessageError::WrongRequestId},
            // Body lengths.
            {"a Probe with a 1-byte body", frame(MessageType::Probe, 1, {std::byte{0}}, 1),
             MessageError::LongBody},
            {"a short Probed", frame(MessageType::Probed, 1, shortened(probed_body(44'100, 2, 1))),
             MessageError::ShortBody},
            {"a short Decode", frame(MessageType::Decode, 1, shortened(decode_body(5)), 1),
             MessageError::ShortBody},
            {"a short Decoded", frame(MessageType::Decoded, 1, shortened(decoded_body(5, 1))),
             MessageError::ShortBody},
            {"a long Probed", frame(MessageType::Probed, 1, lengthened(probed_body(44'100, 2, 1))),
             MessageError::LongBody},
            {"a long Decode", frame(MessageType::Decode, 1, lengthened(decode_body(5)), 1),
             MessageError::LongBody},
            {"a long Decoded", frame(MessageType::Decoded, 1, lengthened(decoded_body(5, 1))),
             MessageError::LongBody},
        };
        for (const Case& c : cases) {
            CAPTURE(c.name);
            const std::expected<Message, MessageError> decoded = decode(c.frame);
            REQUIRE_FALSE(decoded.has_value());
            CHECK(decoded.error() == c.error);
        }

        // encode() makes the same checks on fields.
        for (const Message& bad : std::vector<Message>{
                 {.request_id = 1,
                  .body = msg::Probed{.sample_rate = 0, .channels = 2, .estimated_frames = 0}},
                 {.request_id = 1,
                  .body =
                      msg::Probed{.sample_rate = 44'100, .channels = 65, .estimated_frames = 0}},
                 {.request_id = 1, .body = msg::Decode{.max_frames = 0}},
                 {.request_id = 1,
                  .body = msg::Decoded{.frames = 1, .status = TrackStatus::Decoding}},
                 {.request_id = 1,
                  .body =
                      msg::Decoded{.frames = kMaxTrackFrames + 1, .status = TrackStatus::Complete}},
             }) {
            const std::expected<Frame, MessageError> encoded = encode(bad);
            REQUIRE_FALSE(encoded.has_value());
            CHECK(encoded.error() == MessageError::BadField);
        }
    }
}
