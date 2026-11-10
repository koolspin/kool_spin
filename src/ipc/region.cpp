// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/region.h>

#include <utility>

namespace ks::ipc {

// ---- Stubs ------------------------------------------------------------------
// The acceptance tests compile against this API before it's implemented
// (specs/shm-primitives/spec.md). These stubs make every test fail.

void write_header(std::span<std::byte> /*segment*/, SegmentKind /*kind*/,
                  std::uint32_t /*layout_version*/) noexcept {}

std::expected<void, HeaderError> check_header(std::span<const std::byte> /*segment*/,
                                              SegmentKind /*kind*/,
                                              std::uint32_t /*layout_version*/) noexcept {
    return std::unexpected{HeaderError::TooSmall};
}

std::expected<Region, std::error_code> Region::create_anonymous(std::size_t /*size*/) {
    return std::unexpected{std::make_error_code(std::errc::function_not_supported)};
}

Region::Region(std::byte* data, std::size_t size) noexcept : data_{data}, size_{size} {}

Region::Region(Region&& other) noexcept
    : data_{std::exchange(other.data_, nullptr)}, size_{std::exchange(other.size_, 0)} {}

Region& Region::operator=(Region&& other) noexcept {
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
    return *this;
}

Region::~Region() {
    data_ = nullptr; // Stub: unmaps the memory once implemented.
}

std::span<std::byte> Region::bytes() const noexcept {
    return {data_, size_};
}

} // namespace ks::ipc
