// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Frames: the unit of the socket protocol (ADR 0003 §6,
/// specs/socket-protocol/spec.md requirements 3 to 10).
///
/// A frame is a 16-byte header, then a body. The header's fields are
/// little-endian:
///
/// | Offset | Bytes | Field            |
/// |--------|-------|------------------|
/// | 0      | 2     | Version          |
/// | 2      | 2     | Type             |
/// | 4      | 4     | Body length      |
/// | 8      | 4     | Request ID       |
/// | 12     | 1     | Descriptor count |
/// | 13     | 3     | Reserved: zero   |
///
/// A frame's descriptors go with its first sendmsg(), and a reader never reads
/// past the end of the frame it's reading, so the descriptors that arrive
/// while reading a frame are that frame's.

#include <ks/ipc/fd_passing.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace ks::ipc {

/// The protocol version every kool_spin program speaks. Changing the header,
/// or any message type's body, means a new version.
inline constexpr std::uint16_t kProtocolVersion = 1;

/// The size of a frame's header.
inline constexpr std::size_t kFrameHeaderSize = 16;

/// The largest body a frame may have.
inline constexpr std::size_t kMaxBodySize = 65536;

/// A frame's header fields and body. Its descriptors travel separately: see
/// ReceivedFrame.
struct Frame {
    std::uint16_t type = 0;
    std::uint32_t request_id = 0;
    std::uint8_t descriptor_count = 0;
    std::vector<std::byte> body;

    friend bool operator==(const Frame&, const Frame&) = default;
};

/// Why a frame was refused, or couldn't be encoded.
enum class FrameError : std::uint8_t {
    BadVersion,  ///< Not kProtocolVersion.
    BadType,     ///< Type 0.
    TooLong,     ///< A body longer than kMaxBodySize.
    TooManyFds,  ///< A descriptor count over kMaxFdsPerMessage.
    BadReserved, ///< A reserved byte isn't zero.
    FdMismatch,  ///< The descriptors that arrived don't match the header's count.
};

/// Reads frames from a stream, one at a time, with no I/O of its own, so it
/// can be fuzzed (requirement 9). The caller reads into next(), then reports
/// what arrived with commit().
class FrameReader {
public:
    /// The buffer to read into next: the rest of the header, or the rest of
    /// the body. Never empty while a frame is incomplete; empty once a frame
    /// has been refused.
    [[nodiscard]] std::span<std::byte> next() noexcept;

    /// Records that the first `bytes` bytes of next() arrived, with
    /// `descriptors` descriptors. Returns the frame once it's complete, nothing
    /// while it isn't, or why it was refused. Once a frame has been refused,
    /// every later call returns the same error.
    ///
    /// The header is checked as soon as its 16 bytes have arrived, before the
    /// body is allocated. A frame whose descriptors don't match its header's
    /// count is refused with FdMismatch.
    ///
    /// @pre bytes <= next().size()
    [[nodiscard]] std::expected<std::optional<Frame>, FrameError> commit(std::size_t bytes,
                                                                         std::size_t descriptors);

    /// True if nothing of the next frame has arrived yet: the stream is at a
    /// frame boundary.
    [[nodiscard]] bool at_boundary() const noexcept;

private:
    std::array<std::byte, kFrameHeaderSize> header_{};
    std::size_t header_filled_ = 0;
    std::size_t body_filled_ = 0;
    std::size_t descriptors_ = 0;
    Frame frame_;
    std::optional<FrameError> failed_;
};

/// The bytes of a frame: its header, then `body`. Refuses type 0 (BadType), a
/// body longer than kMaxBodySize (TooLong), and more than kMaxFdsPerMessage
/// descriptors (TooManyFds).
[[nodiscard]] std::expected<std::vector<std::byte>, FrameError>
encode_frame(std::uint16_t type, std::uint32_t request_id, std::span<const std::byte> body,
             std::size_t descriptor_count);

} // namespace ks::ipc
