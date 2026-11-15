// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Creating shared-memory segments that have no names (ADR 0003 §7,
/// specs/segments/spec.md requirements 6 to 12). Region::map() maps them.

#include <ks/ipc/fd.h>
#include <ks/ipc/region.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>

namespace ks::ipc {

/// macOS: the file in the runtime directory that holds the name of a
/// shared-memory object between creating and unlinking it.
inline constexpr std::string_view kIntentFileName = "segments.intent";

/// macOS: every shared-memory object's name is this prefix and 16 letters
/// and digits, 27 characters in all (macOS allows 31).
inline constexpr std::string_view kSegmentNamePrefix = "/kool_spin-";

/// macOS: how many random names create() tries before giving up.
inline constexpr int kMaxSegmentNameAttempts = 8;

/// Creates every segment. Move-only.
class SegmentFactory {
public:
    /// Prepares to create segments for the runtime directory `runtime_dir`,
    /// which must already exist (open_runtime_directory()).
    ///
    /// On macOS it locks the directory's intent file until the factory is
    /// destroyed, and unlinks a shared-memory object whose name a crash left
    /// in the file. It refuses, with std::errc::device_or_resource_busy, a
    /// directory whose intent file another factory has locked.
    ///
    /// On Linux it creates no file, and any number of factories can share a
    /// directory.
    [[nodiscard]] static std::expected<SegmentFactory, std::error_code>
    open(const std::filesystem::path& runtime_dir);

    SegmentFactory(const SegmentFactory&) = delete;
    SegmentFactory& operator=(const SegmentFactory&) = delete;
    SegmentFactory(SegmentFactory&&) noexcept = default;
    SegmentFactory& operator=(SegmentFactory&&) noexcept = default;
    ~SegmentFactory() = default;

    /// Creates a segment of `size` bytes rounded up to the page size,
    /// zero-filled apart from a header for `kind` and `layout_version`, and
    /// returns its descriptor, close-on-exec. By then the segment has no name
    /// and can't change size: on Linux it's a sealed memfd, on macOS an
    /// unlinked POSIX shared-memory object.
    ///
    /// Refuses a `size` of zero (std::errc::invalid_argument) and one that
    /// overflows when rounded up (std::errc::value_too_large).
    [[nodiscard]] std::expected<Fd, std::error_code>
    create(SegmentKind kind, std::uint32_t layout_version, std::size_t size);

private:
    explicit SegmentFactory(Fd intent) noexcept : intent_{std::move(intent)} {}

    Fd intent_; ///< macOS: the locked intent file. Linux: nothing.
};

} // namespace ks::ipc
