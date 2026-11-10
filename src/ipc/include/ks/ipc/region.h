// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Shared-memory regions and the header every segment starts with
/// (ADR 0003 §7).

#include <ks/ipc/layout.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <system_error>
#include <type_traits>

namespace ks::ipc {

/// What a segment holds.
enum class SegmentKind : std::uint8_t {
    EngineState = 1,
    CommandRing = 2,
    Track = 3,
    Log = 4,
};

/// "KOOLSPIN" in memory, on little-endian machines.
inline constexpr std::uint64_t kSegmentMagic = 0x4E49'5053'4C4F'4F4BULL;

/// Bytes reserved for the header at the start of every segment. Layouts in
/// the segment start at this offset or later.
inline constexpr std::size_t kSegmentHeaderSize = kSharedAlignment;

/// The first bytes of every segment. The segment's creator writes it once,
/// before any other process can see the segment.
struct SegmentHeader {
    std::uint64_t magic;          ///< kSegmentMagic.
    std::uint32_t layout_version; ///< Changes whenever the layout after the header does.
    std::uint32_t kind;           ///< A SegmentKind, kept as an integer: another process wrote it.
    std::uint64_t size;           ///< The segment's size in bytes, header included.
};

static_assert(sizeof(SegmentHeader) <= kSegmentHeaderSize);

/// Why check_header() refused a segment.
enum class HeaderError : std::uint8_t {
    TooSmall,   ///< The segment is smaller than a header.
    BadMagic,   ///< Not a kool_spin segment.
    BadVersion, ///< A different layout version.
    BadKind,    ///< A different kind, or not a SegmentKind at all.
    BadSize,    ///< The header's size doesn't match the caller's mapping.
};

/// Writes a header at the start of `segment`, recording `segment.size()`.
/// @pre segment.size() >= kSegmentHeaderSize
void write_header(std::span<std::byte> segment, SegmentKind kind,
                  std::uint32_t layout_version) noexcept;

/// Copies the header out of `segment` and checks it against what the caller
/// expects. `segment` must be the caller's own mapping: its size is trusted,
/// and the header's size field is checked against it.
[[nodiscard]] std::expected<void, HeaderError> check_header(std::span<const std::byte> segment,
                                                            SegmentKind kind,
                                                            std::uint32_t layout_version) noexcept;

/// A mapping of shared memory, unmapped when the Region is destroyed.
class Region {
public:
    /// Maps zero-filled memory that children created with fork() share. The
    /// mapping is at least `size` bytes, rounded up to the page size. Returns
    /// an error if `size` is zero or the mapping fails.
    [[nodiscard]] static std::expected<Region, std::error_code> create_anonymous(std::size_t size);

    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;
    Region(Region&& other) noexcept;
    Region& operator=(Region&& other) noexcept;
    ~Region();

    /// The whole mapping.
    [[nodiscard]] std::span<std::byte> bytes() const noexcept;

    /// Returns the `T` at `offset`, or null if it wouldn't fit inside the
    /// region (including when `offset + sizeof(T)` overflows) or `offset`
    /// isn't aligned for `T`.
    template <typename T>
    [[nodiscard]] T* at(std::size_t offset) const noexcept;

private:
    Region(std::byte* data, std::size_t size) noexcept;

    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

// ---- Stubs ------------------------------------------------------------------
// The acceptance tests compile against this API before it's implemented
// (specs/shm-primitives/spec.md). These stubs make every test fail.

template <typename T>
T* Region::at(std::size_t /*offset*/) const noexcept {
    static_assert(std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>,
                  "only shared layouts live in a region");
    return nullptr;
}

} // namespace ks::ipc
