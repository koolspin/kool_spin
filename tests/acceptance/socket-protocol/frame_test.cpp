// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/socket-protocol/spec.md: frames and FrameReader.

#include <ks/ipc/frame.h>

#include <ks/ipc/fd_passing.h>

#include "protocol_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

namespace {

using ks::ipc::encode_frame;
using ks::ipc::Frame;
using ks::ipc::FrameError;
using ks::ipc::FrameReader;
using ks::ipc::kFrameHeaderSize;
using ks::ipc::kMaxBodySize;
using ks::ipc::kMaxFdsPerMessage;
using ks::ipc::test::bytes;
using ks::ipc::test::pattern;

/// What feed() saw.
struct Fed {
    std::vector<Frame> frames;
    std::optional<FrameError> error;
    bool offered_past_frame = false; ///< next() offered more than the rest of a frame.
    bool stalled = false;            ///< next() was empty with no error reported.
};

/// Feeds `stream` to `reader`, at most `chunk()` bytes at a time. `descriptors`
/// gives each frame's descriptor count, reported with its first bytes; frames
/// past its end get 0. `frame_sizes`, if given, is each frame's size in bytes,
/// so feed() can check that next() never offers more than the rest of one.
Fed feed(FrameReader& reader, std::span<const std::byte> stream,
         const std::function<std::size_t()>& chunk, std::span<const std::size_t> descriptors = {},
         std::span<const std::size_t> frame_sizes = {}) {
    Fed fed;
    std::size_t offset = 0;
    std::size_t frame_index = 0;
    std::size_t frame_end = frame_sizes.empty() ? stream.size() : frame_sizes.front();
    while (offset < stream.size()) {
        const std::span<std::byte> buffer = reader.next();
        if (buffer.empty()) {
            fed.stalled = true;
            break;
        }
        if (buffer.size() > frame_end - offset) {
            fed.offered_past_frame = true;
        }
        const std::size_t count = std::min({buffer.size(), chunk(), stream.size() - offset});
        std::size_t fds = 0;
        if (reader.at_boundary() && frame_index < descriptors.size()) {
            fds = descriptors[frame_index];
        }
        std::ranges::copy(stream.subspan(offset, count), buffer.begin());
        offset += count;
        std::expected<std::optional<Frame>, FrameError> result = reader.commit(count, fds);
        if (!result) {
            fed.error = result.error();
            break;
        }
        if (*result) {
            fed.frames.push_back(std::move(**result));
            ++frame_index;
            if (frame_index < frame_sizes.size()) {
                frame_end += frame_sizes[frame_index];
            }
        }
    }
    return fed;
}

/// Reads everything next() offers each time.
std::size_t whole() {
    return kMaxBodySize + kFrameHeaderSize;
}

/// A frame's fields and body.
Frame make_frame(std::uint16_t type, std::uint32_t request_id, std::uint8_t descriptors,
                 std::vector<std::byte> body) {
    return Frame{.type = type,
                 .request_id = request_id,
                 .descriptor_count = descriptors,
                 .body = std::move(body)};
}

/// The bytes of `frame`, or nothing if encode_frame() refuses it.
std::vector<std::byte> encoded(const Frame& frame) {
    return encode_frame(frame.type, frame.request_id, frame.body, frame.descriptor_count)
        .value_or(std::vector<std::byte>{});
}

/// A valid header for a frame of type 7, request ID 1, no descriptors, and a
/// 4-byte body, followed by that body.
std::vector<std::byte> valid_frame_bytes() {
    return bytes({0x01, 0x00, 0x07, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00,
                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA1, 0xA2, 0xA3, 0xA4});
}

/// Sets the bytes from `offset` on in `stream` to `values`.
void poke(std::vector<std::byte>& stream, std::size_t offset, const std::vector<int>& values) {
    for (const int value : values) {
        stream.at(offset) = static_cast<std::byte>(value);
        ++offset;
    }
}

} // namespace

TEST_CASE("Frames: the header's layout", "[frames]") {
    const std::vector<std::byte> expected = bytes({
        0x01, 0x00,             // version 1
        0x34, 0x12,             // type 0x1234
        0x03, 0x00, 0x00, 0x00, // body length 3
        0xEF, 0xCD, 0xAB, 0x89, // request ID 0x89ABCDEF
        0x02,                   // 2 descriptors
        0x00, 0x00, 0x00,       // reserved
        0xAA, 0xBB, 0xCC,       // the body
    });
    const Frame frame = make_frame(0x1234, 0x89AB'CDEF, 2, bytes({0xAA, 0xBB, 0xCC}));
    REQUIRE(encoded(frame) == expected);

    FrameReader reader;
    REQUIRE(reader.at_boundary());
    const std::array<std::size_t, 1> descriptors{2};
    const Fed fed = feed(reader, expected, whole, descriptors);
    REQUIRE_FALSE(fed.error.has_value());
    REQUIRE(fed.frames.size() == 1);
    REQUIRE(fed.frames.front() == frame);
    REQUIRE(reader.at_boundary());

    // An empty body: the frame is complete with its header.
    const Frame empty = make_frame(1, 0, 0, {});
    const std::vector<std::byte> header = encoded(empty);
    REQUIRE(header == bytes({0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                             0x00, 0x00, 0x00, 0x00}));
    const Fed fed_empty = feed(reader, header, whole);
    REQUIRE(fed_empty.frames.size() == 1);
    REQUIRE(fed_empty.frames.front() == empty);
}

TEST_CASE("Frames: every bad header is refused", "[frames]") {
    struct Case {
        const char* what;
        std::size_t offset;
        std::vector<int> values;
        FrameError error;
    };
    const std::vector<Case> cases{
        {"version 0", 0, {0x00, 0x00}, FrameError::BadVersion},
        {"version 2", 0, {0x02, 0x00}, FrameError::BadVersion},
        {"type 0", 2, {0x00, 0x00}, FrameError::BadType},
        {"body length 65,537", 4, {0x01, 0x00, 0x01, 0x00}, FrameError::TooLong},
        {"body length 2^32 - 1", 4, {0xFF, 0xFF, 0xFF, 0xFF}, FrameError::TooLong},
        {"9 descriptors", 12, {0x09}, FrameError::TooManyFds},
        {"reserved byte 13", 13, {0x01}, FrameError::BadReserved},
        {"reserved byte 14", 14, {0x01}, FrameError::BadReserved},
        {"reserved byte 15", 15, {0x80}, FrameError::BadReserved},
    };
    for (const Case& bad : cases) {
        CAPTURE(bad.what);
        std::vector<std::byte> stream = valid_frame_bytes();
        poke(stream, bad.offset, bad.values);

        // Nothing is refused until the 16th byte arrives, and then at once.
        FrameReader reader;
        REQUIRE(reader.next().size() == kFrameHeaderSize);
        std::ranges::copy(std::span{stream}.first(kFrameHeaderSize - 1), reader.next().begin());
        const std::expected<std::optional<Frame>, FrameError> partial =
            reader.commit(kFrameHeaderSize - 1, 0);
        REQUIRE(partial.has_value());
        REQUIRE_FALSE(partial->has_value());
        REQUIRE(reader.next().size() == 1);
        reader.next().front() = stream.at(kFrameHeaderSize - 1);
        const std::expected<std::optional<Frame>, FrameError> refused = reader.commit(1, 0);
        REQUIRE_FALSE(refused.has_value());
        REQUIRE(refused.error() == bad.error);

        // No buffer for the body, and the same refusal from then on.
        REQUIRE(reader.next().empty());
        const std::expected<std::optional<Frame>, FrameError> again = reader.commit(0, 0);
        REQUIRE_FALSE(again.has_value());
        REQUIRE(again.error() == bad.error);
    }

    // The limits themselves are accepted: the reader offers the whole body.
    std::vector<std::byte> largest = valid_frame_bytes();
    poke(largest, 4, {0x00, 0x00, 0x01, 0x00});
    poke(largest, 12, {0x08});
    FrameReader reader;
    std::ranges::copy(std::span{largest}.first(kFrameHeaderSize), reader.next().begin());
    const std::expected<std::optional<Frame>, FrameError> accepted =
        reader.commit(kFrameHeaderSize, kMaxFdsPerMessage);
    REQUIRE(accepted.has_value());
    REQUIRE_FALSE(accepted->has_value());
    REQUIRE(reader.next().size() == kMaxBodySize);
}

TEST_CASE("Frames: read in any split", "[frames]") {
    const std::vector<Frame> frames{
        make_frame(1, 0, 0, {}),
        make_frame(2, 5, 1, bytes({0x42})),
        make_frame(3, 7, 8, pattern(kMaxBodySize, 3)),
        make_frame(0xFFFF, 0xFFFF'FFFF, 0, pattern(100, 9)),
        make_frame(4, 1, 3, {}),
    };
    std::vector<std::byte> stream;
    std::vector<std::size_t> descriptors;
    std::vector<std::size_t> sizes;
    for (const Frame& frame : frames) {
        const std::vector<std::byte> one = encoded(frame);
        REQUIRE_FALSE(one.empty());
        stream.insert(stream.end(), one.begin(), one.end());
        descriptors.push_back(frame.descriptor_count);
        sizes.push_back(one.size());
    }

    const auto check = [&](const std::function<std::size_t()>& chunk) {
        FrameReader reader;
        const Fed fed = feed(reader, stream, chunk, descriptors, sizes);
        REQUIRE_FALSE(fed.error.has_value());
        REQUIRE_FALSE(fed.stalled);
        REQUIRE_FALSE(fed.offered_past_frame);
        REQUIRE(fed.frames == frames);
        REQUIRE(reader.at_boundary());
    };

    SECTION("one byte at a time") {
        check([] { return std::size_t{1}; });
    }
    SECTION("everything offered") {
        check(whole);
    }
    SECTION("random sizes") {
        for (const std::uint32_t seed : {1U, 2U, 3U, 4U, 5U}) {
            CAPTURE(seed);
            std::mt19937 random{seed};
            std::uniform_int_distribution<std::size_t> size{1, 40'000};
            check([&] { return size(random); });
        }
    }
}

TEST_CASE("Frames: descriptor counts must match", "[frames]") {
    struct Case {
        const char* what;
        std::uint8_t declared;
        std::size_t body_size;
        std::size_t first; ///< Descriptors with the first byte.
        std::size_t last;  ///< Descriptors with the last byte.
        bool accepted;
    };
    const std::vector<Case> cases{
        {"2 declared, 1 arrives", 2, 10, 1, 0, false},
        {"2 declared, 3 arrive", 2, 10, 3, 0, false},
        {"none declared, 1 arrives", 0, 10, 1, 0, false},
        {"none declared, 1 arrives last", 0, 10, 0, 1, false},
        {"1 declared, none arrive, empty body", 1, 0, 0, 0, false},
        {"2 declared, 1 first and 1 last", 2, 10, 1, 1, true},
        {"8 declared, 8 arrive", 8, 10, 8, 0, true},
    };
    for (const Case& test : cases) {
        CAPTURE(test.what);
        const std::vector<std::byte> stream =
            encoded(make_frame(9, 0, test.declared, pattern(test.body_size, 0)));
        REQUIRE_FALSE(stream.empty());

        // One byte at a time, so the first and last bytes are separate commits.
        FrameReader reader;
        std::optional<FrameError> error;
        std::size_t frames = 0;
        for (std::size_t i = 0; i < stream.size() && !error; ++i) {
            REQUIRE_FALSE(reader.next().empty());
            reader.next().front() = stream.at(i);
            std::size_t fds = 0;
            if (i == 0) {
                fds += test.first;
            }
            if (i + 1 == stream.size()) {
                fds += test.last;
            }
            const std::expected<std::optional<Frame>, FrameError> result = reader.commit(1, fds);
            if (!result) {
                error = result.error();
            } else if (*result) {
                ++frames;
            }
        }
        if (test.accepted) {
            REQUIRE_FALSE(error.has_value());
            REQUIRE(frames == 1);
        } else {
            REQUIRE(error == FrameError::FdMismatch);
            REQUIRE(frames == 0);
        }
    }
}

TEST_CASE("Frames: what can't be encoded", "[frames]") {
    const std::vector<std::byte> small = bytes({1, 2, 3});
    const std::vector<std::byte> too_long(kMaxBodySize + 1);
    const std::vector<std::byte> longest(kMaxBodySize);

    const std::expected<std::vector<std::byte>, FrameError> type_zero =
        encode_frame(0, 1, small, 0);
    REQUIRE_FALSE(type_zero.has_value());
    REQUIRE(type_zero.error() == FrameError::BadType);

    const std::expected<std::vector<std::byte>, FrameError> long_body =
        encode_frame(1, 1, too_long, 0);
    REQUIRE_FALSE(long_body.has_value());
    REQUIRE(long_body.error() == FrameError::TooLong);

    const std::expected<std::vector<std::byte>, FrameError> nine =
        encode_frame(1, 1, small, kMaxFdsPerMessage + 1);
    REQUIRE_FALSE(nine.has_value());
    REQUIRE(nine.error() == FrameError::TooManyFds);

    const std::expected<std::vector<std::byte>, FrameError> limits =
        encode_frame(0xFFFF, 0xFFFF'FFFF, longest, kMaxFdsPerMessage);
    REQUIRE(limits.has_value());
    REQUIRE(limits->size() == kFrameHeaderSize + kMaxBodySize);
}
