// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/frame.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace ks::ipc {

// Stubs: specs/socket-protocol/spec.md isn't implemented yet.

std::span<std::byte> FrameReader::next() noexcept {
    return std::span{header_}.first(0);
}

std::expected<std::optional<Frame>, FrameError> FrameReader::commit(std::size_t /*bytes*/,
                                                                    std::size_t /*descriptors*/) {
    failed_ = FrameError::BadVersion;
    return std::unexpected{FrameError::BadVersion};
}

bool FrameReader::at_boundary() const noexcept {
    // Uses the state the real reader keeps, so the stub compiles cleanly.
    return header_filled_ == 0 && body_filled_ == 0 && descriptors_ == 0 &&
           header_.front() == std::byte{0};
}

std::expected<std::vector<std::byte>, FrameError> encode_frame(std::uint16_t /*type*/,
                                                               std::uint32_t /*request_id*/,
                                                               std::span<const std::byte> /*body*/,
                                                               std::size_t /*descriptor_count*/) {
    return std::unexpected{FrameError::BadType};
}

} // namespace ks::ipc
