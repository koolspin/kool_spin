// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/track-loading/spec.md: the message types it adds
// (requirement 1).

#include <ks/ipc/messages.h>

#include <ks/ipc/frame.h>
#include <ks/ipc/track_segment.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ks::ipc::decode;
using ks::ipc::encode;
using ks::ipc::Frame;
using ks::ipc::kMaxPathSize;
using ks::ipc::kMaxTrackFrames;
using ks::ipc::kMaxTrackRate;
using ks::ipc::kMinTrackRate;
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

/// Appends `value` to `body`, little-endian, in `size` bytes.
void append(std::vector<std::byte>& body, std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        body.push_back(static_cast<std::byte>(value & 0xFFU));
        value >>= 8U;
    }
}

/// A LoadFile's body, written out by hand: the deck, then the path as text.
std::vector<std::byte> load_file_body(std::uint64_t deck, std::string_view path) {
    std::vector<std::byte> body;
    append(body, deck, 4);
    append(body, path.size(), 4);
    for (const char c : path) {
        body.push_back(static_cast<std::byte>(c));
    }
    return body;
}

/// A FileLoaded's body, written out by hand.
std::vector<std::byte> file_loaded_body(std::uint64_t track_id, std::uint64_t rate,
                                        std::uint64_t estimated) {
    std::vector<std::byte> body;
    append(body, track_id, 8);
    append(body, rate, 4);
    append(body, estimated, 8);
    return body;
}

/// A LoadTrack's body, written out by hand.
std::vector<std::byte> load_track_body(std::uint64_t track_id, std::uint64_t deck,
                                       std::uint64_t rate) {
    std::vector<std::byte> body;
    append(body, track_id, 8);
    append(body, deck, 4);
    append(body, rate, 4);
    return body;
}

/// A TrackReleased's body, written out by hand.
std::vector<std::byte> track_released_body(std::uint64_t track_id) {
    std::vector<std::byte> body;
    append(body, track_id, 8);
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

/// A path of exactly `size` bytes: "/", then "é" (two bytes) as often as fits,
/// then "a" if a byte is left over.
std::string path_of_size(std::size_t size) {
    std::string path = "/";
    while (path.size() + 2 <= size) {
        path += "\xC3\xA9";
    }
    if (path.size() < size) {
        path += 'a';
    }
    return path;
}

constexpr std::uint64_t kLargestId = std::numeric_limits<std::uint64_t>::max();

} // namespace

TEST_CASE("Track messages: round trip", "[messages]") {
    SECTION("the numbers and the limit") {
        CHECK(static_cast<int>(MessageType::LoadFile) == 19);
        CHECK(static_cast<int>(MessageType::FileLoaded) == 20);
        CHECK(static_cast<int>(MessageType::LoadTrack) == 21);
        CHECK(static_cast<int>(MessageType::TrackLoaded) == 22);
        CHECK(static_cast<int>(MessageType::TrackReleased) == 23);
        CHECK(kMaxPathSize == 4'096);
    }

    SECTION("round trips") {
        const std::string longest = path_of_size(4'096);
        REQUIRE(longest.size() == 4'096);
        const std::vector<Message> messages{
            {.request_id = 1, .body = msg::LoadFile{.deck = 0, .path = "/"}},
            {.request_id = 2, .body = msg::LoadFile{.deck = 3, .path = longest}},
            {.request_id = 3,
             .body = msg::FileLoaded{.track_id = 1, .sample_rate = 44'100, .estimated_frames = 0}},
            {.request_id = 4,
             .body = msg::FileLoaded{.track_id = kLargestId,
                                     .sample_rate = 96'000,
                                     .estimated_frames = kMaxTrackFrames}},
            {.request_id = 5,
             .body = msg::LoadTrack{.track_id = 7, .deck = 0, .sample_rate = kMinTrackRate}},
            {.request_id = 6,
             .body =
                 msg::LoadTrack{.track_id = kLargestId, .deck = 3, .sample_rate = kMaxTrackRate}},
            {.request_id = 7, .body = msg::TrackLoaded{}},
            {.request_id = 0, .body = msg::TrackReleased{.track_id = kLargestId}},
            {.request_id = 0, .body = msg::TrackReleased{.track_id = 1}},
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
        const auto load_file =
            encode(Message{.request_id = 9, .body = msg::LoadFile{.deck = 2, .path = "/a.flac"}});
        REQUIRE(load_file.has_value());
        CHECK(*load_file == frame(MessageType::LoadFile, 9, load_file_body(2, "/a.flac")));

        const auto file_loaded = encode(Message{
            .request_id = 9,
            .body = msg::FileLoaded{
                .track_id = 0x1'0000'0002, .sample_rate = 48'000, .estimated_frames = 300'000}});
        REQUIRE(file_loaded.has_value());
        CHECK(*file_loaded ==
              frame(MessageType::FileLoaded, 9, file_loaded_body(0x1'0000'0002, 48'000, 300'000)));

        const auto load_track = encode(Message{
            .request_id = 10,
            .body = msg::LoadTrack{.track_id = 0x1'0000'0003, .deck = 1, .sample_rate = 44'100}});
        REQUIRE(load_track.has_value());
        CHECK(*load_track ==
              frame(MessageType::LoadTrack, 10, load_track_body(0x1'0000'0003, 1, 44'100), 1));

        const auto track_loaded = encode(Message{.request_id = 10, .body = msg::TrackLoaded{}});
        REQUIRE(track_loaded.has_value());
        CHECK(*track_loaded == frame(MessageType::TrackLoaded, 10, {}));

        const auto released =
            encode(Message{.request_id = 0, .body = msg::TrackReleased{.track_id = 0x1'0000'0004}});
        REQUIRE(released.has_value());
        CHECK(*released ==
              frame(MessageType::TrackReleased, 0, track_released_body(0x1'0000'0004)));
    }
}

TEST_CASE("Track messages: what's refused", "[messages]") {
    struct Case {
        const char* name;
        Frame frame;
        MessageError error;
    };
    const std::vector<std::byte> file = load_file_body(0, "/a.flac");
    const std::vector<std::byte> loaded = file_loaded_body(1, 44'100, 44'100);
    const std::vector<std::byte> track = load_track_body(1, 0, 44'100);
    const std::vector<std::byte> released = track_released_body(1);
    const std::vector<Case> cases{
        // LoadFile's fields.
        {"a LoadFile for deck 4", frame(MessageType::LoadFile, 1, load_file_body(4, "/a.flac")),
         MessageError::BadField},
        {"a LoadFile with an empty path", frame(MessageType::LoadFile, 1, load_file_body(0, "")),
         MessageError::BadField},
        {"a LoadFile with a 4,097-byte path",
         frame(MessageType::LoadFile, 1, load_file_body(0, path_of_size(4'097))),
         MessageError::BadField},
        {"a LoadFile with a relative path",
         frame(MessageType::LoadFile, 1, load_file_body(0, "music/a.flac")),
         MessageError::BadField},
        // Track IDs of 0.
        {"a FileLoaded for track 0",
         frame(MessageType::FileLoaded, 1, file_loaded_body(0, 44'100, 1)), MessageError::BadField},
        {"a LoadTrack for track 0",
         frame(MessageType::LoadTrack, 1, load_track_body(0, 0, 44'100), 1),
         MessageError::BadField},
        {"a TrackReleased for track 0",
         frame(MessageType::TrackReleased, 0, track_released_body(0)), MessageError::BadField},
        // Rates, the estimate, and LoadTrack's deck.
        {"a FileLoaded at 7,999 Hz",
         frame(MessageType::FileLoaded, 1, file_loaded_body(1, 7'999, 1)), MessageError::BadField},
        {"a FileLoaded at 384,001 Hz",
         frame(MessageType::FileLoaded, 1, file_loaded_body(1, 384'001, 1)),
         MessageError::BadField},
        {"a LoadTrack at 7,999 Hz",
         frame(MessageType::LoadTrack, 1, load_track_body(1, 0, 7'999), 1), MessageError::BadField},
        {"a LoadTrack at 384,001 Hz",
         frame(MessageType::LoadTrack, 1, load_track_body(1, 0, 384'001), 1),
         MessageError::BadField},
        {"a FileLoaded estimating more than the cap",
         frame(MessageType::FileLoaded, 1, file_loaded_body(1, 44'100, kMaxTrackFrames + 1)),
         MessageError::BadField},
        {"a LoadTrack for deck 4",
         frame(MessageType::LoadTrack, 1, load_track_body(1, 4, 44'100), 1),
         MessageError::BadField},
        // Descriptors.
        {"a LoadTrack with no descriptor", frame(MessageType::LoadTrack, 1, track, 0),
         MessageError::WrongFdCount},
        {"a LoadTrack with 2 descriptors", frame(MessageType::LoadTrack, 1, track, 2),
         MessageError::WrongFdCount},
        {"a LoadFile with a descriptor", frame(MessageType::LoadFile, 1, file, 1),
         MessageError::WrongFdCount},
        {"a FileLoaded with a descriptor", frame(MessageType::FileLoaded, 1, loaded, 1),
         MessageError::WrongFdCount},
        {"a TrackLoaded with a descriptor", frame(MessageType::TrackLoaded, 1, {}, 1),
         MessageError::WrongFdCount},
        {"a TrackReleased with a descriptor", frame(MessageType::TrackReleased, 0, released, 1),
         MessageError::WrongFdCount},
        // Request IDs.
        {"a LoadFile with request ID 0", frame(MessageType::LoadFile, 0, file),
         MessageError::WrongRequestId},
        {"a FileLoaded with request ID 0", frame(MessageType::FileLoaded, 0, loaded),
         MessageError::WrongRequestId},
        {"a LoadTrack with request ID 0", frame(MessageType::LoadTrack, 0, track, 1),
         MessageError::WrongRequestId},
        {"a TrackLoaded with request ID 0", frame(MessageType::TrackLoaded, 0, {}),
         MessageError::WrongRequestId},
        {"a TrackReleased with request ID 1", frame(MessageType::TrackReleased, 1, released),
         MessageError::WrongRequestId},
        // Body lengths.
        {"a short LoadFile", frame(MessageType::LoadFile, 1, shortened(file)),
         MessageError::ShortBody},
        {"a long LoadFile", frame(MessageType::LoadFile, 1, lengthened(file)),
         MessageError::LongBody},
        {"a short FileLoaded", frame(MessageType::FileLoaded, 1, shortened(loaded)),
         MessageError::ShortBody},
        {"a long FileLoaded", frame(MessageType::FileLoaded, 1, lengthened(loaded)),
         MessageError::LongBody},
        {"a short LoadTrack", frame(MessageType::LoadTrack, 1, shortened(track), 1),
         MessageError::ShortBody},
        {"a long LoadTrack", frame(MessageType::LoadTrack, 1, lengthened(track), 1),
         MessageError::LongBody},
        {"a TrackLoaded with a 1-byte body", frame(MessageType::TrackLoaded, 1, {std::byte{0}}),
         MessageError::LongBody},
        {"a short TrackReleased", frame(MessageType::TrackReleased, 0, shortened(released)),
         MessageError::ShortBody},
        {"a long TrackReleased", frame(MessageType::TrackReleased, 0, lengthened(released)),
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
        {"deck 4",
         {.request_id = 1, .body = msg::LoadFile{.deck = 4, .path = "/a"}},
         MessageError::BadField},
        {"an empty path",
         {.request_id = 1, .body = msg::LoadFile{.deck = 0, .path = ""}},
         MessageError::BadField},
        {"a relative path",
         {.request_id = 1, .body = msg::LoadFile{.deck = 0, .path = "a"}},
         MessageError::BadField},
        {"a 4,097-byte path",
         {.request_id = 1, .body = msg::LoadFile{.deck = 0, .path = path_of_size(4'097)}},
         MessageError::BadField},
        {"a FileLoaded for track 0",
         {.request_id = 1,
          .body = msg::FileLoaded{.track_id = 0, .sample_rate = 44'100, .estimated_frames = 0}},
         MessageError::BadField},
        {"a FileLoaded estimating more than the cap",
         {.request_id = 1,
          .body = msg::FileLoaded{.track_id = 1,
                                  .sample_rate = 44'100,
                                  .estimated_frames = kMaxTrackFrames + 1}},
         MessageError::BadField},
        {"a LoadTrack at 0 Hz",
         {.request_id = 1, .body = msg::LoadTrack{.track_id = 1, .deck = 0, .sample_rate = 0}},
         MessageError::BadField},
        {"a LoadTrack for deck 4",
         {.request_id = 1, .body = msg::LoadTrack{.track_id = 1, .deck = 4, .sample_rate = 44'100}},
         MessageError::BadField},
        {"a TrackReleased for track 0",
         {.request_id = 0, .body = msg::TrackReleased{.track_id = 0}},
         MessageError::BadField},
        {"a TrackReleased with request ID 1",
         {.request_id = 1, .body = msg::TrackReleased{.track_id = 1}},
         MessageError::WrongRequestId},
        {"a TrackLoaded with request ID 0",
         {.request_id = 0, .body = msg::TrackLoaded{}},
         MessageError::WrongRequestId},
    };
    for (const Bad& b : bad) {
        CAPTURE(b.name);
        const std::expected<Frame, MessageError> encoded = encode(b.message);
        REQUIRE_FALSE(encoded.has_value());
        CHECK(encoded.error() == b.error);
    }
}
