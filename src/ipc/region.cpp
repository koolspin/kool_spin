// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/region.h>

#include <sys/mman.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

namespace ks::ipc {

void write_header(std::span<std::byte> segment, SegmentKind kind,
                  std::uint32_t layout_version) noexcept {
    assert(segment.size() >= kSegmentHeaderSize);
    const SegmentHeader header{
        .magic = kSegmentMagic,
        .layout_version = layout_version,
        .kind = std::to_underlying(kind),
        .size = segment.size(),
    };
    std::memcpy(segment.data(), &header, sizeof header);
}

std::expected<void, HeaderError> check_header(std::span<const std::byte> segment, SegmentKind kind,
                                              std::uint32_t layout_version) noexcept {
    if (segment.size() < kSegmentHeaderSize) {
        return std::unexpected{HeaderError::TooSmall};
    }
    // Copy first: another process could change the header while we check it.
    SegmentHeader header{};
    std::memcpy(&header, segment.data(), sizeof header);

    if (header.magic != kSegmentMagic) {
        return std::unexpected{HeaderError::BadMagic};
    }
    if (header.layout_version != layout_version) {
        return std::unexpected{HeaderError::BadVersion};
    }
    if (header.kind != std::to_underlying(kind)) {
        return std::unexpected{HeaderError::BadKind};
    }
    if (header.size != segment.size()) {
        return std::unexpected{HeaderError::BadSize};
    }
    return {};
}

std::expected<Region, std::error_code> Region::create_anonymous(std::size_t size) {
    if (size == 0) {
        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    }
    const long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        return std::unexpected{std::error_code{errno, std::system_category()}};
    }
    const auto page_size = static_cast<std::size_t>(page);
    if (size > std::numeric_limits<std::size_t>::max() - (page_size - 1)) {
        return std::unexpected{std::make_error_code(std::errc::value_too_large)};
    }
    const std::size_t rounded = (size + page_size - 1) / page_size * page_size;

    // MAP_SHARED: a child created with fork() maps the same pages.
    void* const data =
        ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED) {
        return std::unexpected{std::error_code{errno, std::system_category()}};
    }
    return Region{static_cast<std::byte*>(data), rounded};
}

Region::Region(std::byte* data, std::size_t size) noexcept : data_{data}, size_{size} {}

Region::Region(Region&& other) noexcept
    : data_{std::exchange(other.data_, nullptr)}, size_{std::exchange(other.size_, 0)} {}

Region& Region::operator=(Region&& other) noexcept {
    if (this != &other) {
        unmap();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

Region::~Region() {
    unmap();
}

std::span<std::byte> Region::bytes() const noexcept {
    return {data_, size_};
}

void Region::unmap() noexcept {
    if (data_ != nullptr) {
        // munmap only fails for arguments that mmap couldn't have returned.
        static_cast<void>(::munmap(data_, size_));
        data_ = nullptr;
        size_ = 0;
    }
}

} // namespace ks::ipc
