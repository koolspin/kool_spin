// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The fuzz target's checks (specs/socket-protocol/spec.md requirement 25). They
// live in a header so that "Messages: round trip" compiles, lints, and runs
// them in every preset; the fuzz target runs them on arbitrary input.

#include <ks/ipc/fd_passing.h>
#include <ks/ipc/frame.h>
#include <ks/ipc/messages.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace ks::ipc::test {

/// Chunk sizes and descriptor counts, from a seed (SplitMix64).
class Choices {
public:
    explicit Choices(std::uint64_t seed) noexcept : state_{seed} {}

    /// A number from 0 to `bound` - 1. @pre bound > 0
    std::size_t below(std::size_t bound) noexcept {
        state_ += 0x9E37'79B9'7F4A'7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D0'49BB'1331'11EBULL;
        z ^= z >> 31U;
        return static_cast<std::size_t>(z % bound);
    }

private:
    std::uint64_t state_;
};

/// The descriptor count to report with the first bytes of the frame at
/// `offset` in `stream`: the header's own count, unless `mismatch`, when it's
/// chosen from 0 to 9.
inline std::size_t descriptors_for(std::span<const std::byte> stream, std::size_t offset,
                                   bool mismatch, Choices& choices) {
    if (mismatch) {
        return choices.below(kMaxFdsPerMessage + 2);
    }
    constexpr std::size_t kCountOffset = 12;
    if (offset + kCountOffset >= stream.size()) {
        return 0;
    }
    return std::to_integer<std::size_t>(stream[offset + kCountOffset]);
}

/// Feeds `stream` through a FrameReader and decodes every frame it completes.
/// `control` chooses how: its top bit reads everything next() offers, and
/// otherwise 1 to 64 bytes at a time; its next bit reports descriptor counts
/// that may not match the headers. True if every property holds:
/// - a refusal empties next() and repeats on the next commit();
/// - every frame completed re-encodes to the bytes it was read from, and
///   leaves the reader at a frame boundary;
/// - every frame that decodes encodes back to the same frame.
inline bool stream_checks_hold(std::uint8_t control, std::span<const std::byte> stream) {
    const bool whole = (control & 0x80U) != 0;
    const bool mismatch = (control & 0x40U) != 0;
    Choices choices{control};
    FrameReader reader;
    std::size_t offset = 0;
    std::size_t frame_start = 0;
    while (offset < stream.size()) {
        const std::span<std::byte> buffer = reader.next();
        if (buffer.empty()) {
            return false;
        }
        const std::size_t limit = whole ? buffer.size() : 1 + choices.below(64);
        const std::size_t count = std::min({buffer.size(), limit, stream.size() - offset});
        std::size_t descriptors = 0;
        if (reader.at_boundary()) {
            frame_start = offset;
            descriptors = descriptors_for(stream, offset, mismatch, choices);
        }
        std::ranges::copy(stream.subspan(offset, count), buffer.begin());
        offset += count;

        const std::expected<std::optional<Frame>, FrameError> result =
            reader.commit(count, descriptors);
        if (!result) {
            const std::expected<std::optional<Frame>, FrameError> again = reader.commit(0, 0);
            return reader.next().empty() && !again && again.error() == result.error();
        }
        if (!*result) {
            continue;
        }
        const Frame& frame = **result;
        const std::expected<std::vector<std::byte>, FrameError> encoded =
            encode_frame(frame.type, frame.request_id, frame.body, frame.descriptor_count);
        if (!encoded ||
            !std::ranges::equal(*encoded, stream.subspan(frame_start, offset - frame_start))) {
            return false;
        }
        if (!reader.at_boundary()) {
            return false;
        }
        const std::expected<Message, MessageError> message = decode(frame);
        if (message) {
            const std::expected<Frame, MessageError> again = encode(*message);
            if (!again || *again != frame) {
                return false;
            }
        }
    }
    return true;
}

/// Takes `input` as a frame's fields: the type (2 bytes, little-endian), the
/// request ID (4), the descriptor count (1), then the body. True if the frame
/// doesn't decode, or decodes to a message that encodes back to the same
/// frame. This reaches the message decoder far more often than a stream of
/// random headers does.
inline bool frame_checks_hold(std::span<const std::byte> input) {
    constexpr std::size_t kFieldsSize = 7;
    if (input.size() < kFieldsSize || input.size() - kFieldsSize > kMaxBodySize) {
        return true;
    }
    const auto byte = [&](std::size_t i) { return std::to_integer<std::uint32_t>(input[i]); };
    Frame frame;
    frame.type = static_cast<std::uint16_t>(byte(0) | (byte(1) << 8U));
    frame.request_id = byte(2) | (byte(3) << 8U) | (byte(4) << 16U) | (byte(5) << 24U);
    frame.descriptor_count = static_cast<std::uint8_t>(byte(6));
    frame.body.assign(input.begin() + kFieldsSize, input.end());

    const std::expected<Message, MessageError> message = decode(frame);
    if (!message) {
        return true;
    }
    const std::expected<Frame, MessageError> again = encode(*message);
    return again && *again == frame && decode(*again) == message;
}

} // namespace ks::ipc::test
