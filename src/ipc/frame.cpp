// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/frame.h>

#include <ks/ipc/fd_passing.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ks::ipc {

namespace {

// The header's fields: offset and size in bytes (requirement 3).
constexpr std::size_t kVersionOffset = 0;
constexpr std::size_t kTypeOffset = 2;
constexpr std::size_t kLengthOffset = 4;
constexpr std::size_t kRequestOffset = 8;
constexpr std::size_t kCountOffset = 12;
constexpr std::size_t kReservedOffset = 13;
constexpr std::size_t kReservedSize = 3;

/// The little-endian integer in `bytes`, at most 4 of them.
std::uint32_t read_le(std::span<const std::byte> bytes) noexcept {
    std::uint32_t value = 0;
    unsigned shift = 0;
    for (const std::byte byte : bytes) {
        value |= std::to_integer<std::uint32_t>(byte) << shift;
        shift += 8U;
    }
    return value;
}

/// Writes `value` little-endian into all of `bytes`.
void write_le(std::span<std::byte> bytes, std::uint32_t value) noexcept {
    for (std::byte& byte : bytes) {
        byte = static_cast<std::byte>(value & 0xFFU);
        value >>= 8U;
    }
}

/// The frame a header describes, with its body allocated, or why the header
/// is refused (requirement 4).
std::expected<Frame, FrameError> parse_header(std::span<const std::byte, kFrameHeaderSize> header) {
    if (read_le(header.subspan(kVersionOffset, 2)) != kProtocolVersion) {
        return std::unexpected{FrameError::BadVersion};
    }
    const std::uint32_t type = read_le(header.subspan(kTypeOffset, 2));
    if (type == 0) {
        return std::unexpected{FrameError::BadType};
    }
    const std::uint32_t length = read_le(header.subspan(kLengthOffset, 4));
    if (length > kMaxBodySize) {
        return std::unexpected{FrameError::TooLong};
    }
    const std::uint32_t count = read_le(header.subspan(kCountOffset, 1));
    if (count > kMaxFdsPerMessage) {
        return std::unexpected{FrameError::TooManyFds};
    }
    if (!std::ranges::all_of(header.subspan(kReservedOffset, kReservedSize),
                             [](std::byte byte) { return byte == std::byte{0}; })) {
        return std::unexpected{FrameError::BadReserved};
    }
    return Frame{
        .type = static_cast<std::uint16_t>(type),
        .request_id = read_le(header.subspan(kRequestOffset, 4)),
        .descriptor_count = static_cast<std::uint8_t>(count),
        .body = std::vector<std::byte>(length),
    };
}

} // namespace

std::span<std::byte> FrameReader::next() noexcept {
    if (failed_) {
        return {};
    }
    if (header_filled_ < kFrameHeaderSize) {
        return std::span{header_}.subspan(header_filled_);
    }
    return std::span{frame_.body}.subspan(body_filled_);
}

std::expected<std::optional<Frame>, FrameError> FrameReader::commit(std::size_t bytes,
                                                                    std::size_t descriptors) {
    if (failed_) {
        return std::unexpected{*failed_};
    }
    const auto fail = [this](FrameError error) {
        failed_ = error;
        return std::unexpected{error};
    };

    descriptors_ += descriptors;
    // `bytes` can't exceed next().size(); clamping keeps a caller's mistake
    // inside the buffers.
    if (header_filled_ < kFrameHeaderSize) {
        header_filled_ += std::min(bytes, kFrameHeaderSize - header_filled_);
        // No header allows more, so don't wait for it to say so.
        if (descriptors_ > kMaxFdsPerMessage) {
            return fail(FrameError::FdMismatch);
        }
        if (header_filled_ < kFrameHeaderSize) {
            return std::nullopt;
        }
        std::expected<Frame, FrameError> parsed = parse_header(header_);
        if (!parsed) {
            return fail(parsed.error());
        }
        frame_ = std::move(*parsed);
    } else {
        body_filled_ += std::min(bytes, frame_.body.size() - body_filled_);
    }

    if (descriptors_ > frame_.descriptor_count) {
        return fail(FrameError::FdMismatch);
    }
    if (body_filled_ < frame_.body.size()) {
        return std::nullopt;
    }
    if (descriptors_ != frame_.descriptor_count) {
        return fail(FrameError::FdMismatch);
    }
    Frame complete = std::exchange(frame_, Frame{});
    header_filled_ = 0;
    body_filled_ = 0;
    descriptors_ = 0;
    return complete;
}

bool FrameReader::at_boundary() const noexcept {
    return header_filled_ == 0 && descriptors_ == 0;
}

std::expected<std::vector<std::byte>, FrameError> encode_frame(std::uint16_t type,
                                                               std::uint32_t request_id,
                                                               std::span<const std::byte> body,
                                                               std::size_t descriptor_count) {
    if (type == 0) {
        return std::unexpected{FrameError::BadType};
    }
    if (body.size() > kMaxBodySize) {
        return std::unexpected{FrameError::TooLong};
    }
    if (descriptor_count > kMaxFdsPerMessage) {
        return std::unexpected{FrameError::TooManyFds};
    }
    std::vector<std::byte> bytes(kFrameHeaderSize + body.size());
    const std::span<std::byte> header = std::span{bytes}.first(kFrameHeaderSize);
    write_le(header.subspan(kVersionOffset, 2), kProtocolVersion);
    write_le(header.subspan(kTypeOffset, 2), type);
    write_le(header.subspan(kLengthOffset, 4), static_cast<std::uint32_t>(body.size()));
    write_le(header.subspan(kRequestOffset, 4), request_id);
    write_le(header.subspan(kCountOffset, 1), static_cast<std::uint32_t>(descriptor_count));
    std::ranges::copy(body, std::span{bytes}.subspan(kFrameHeaderSize).begin());
    return bytes;
}

} // namespace ks::ipc
