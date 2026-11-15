// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/segment.h>

#include <ks/ipc/fd.h>
#include <ks/ipc/region.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <system_error>

namespace ks::ipc {

// Stubs, until the maintainer approves the acceptance tests for
// specs/segments/spec.md.

std::expected<SegmentFactory, std::error_code>
SegmentFactory::open(const std::filesystem::path& /*runtime_dir*/) {
    return std::unexpected{std::make_error_code(std::errc::function_not_supported)};
}

std::expected<Fd, std::error_code> SegmentFactory::create(SegmentKind /*kind*/,
                                                          std::uint32_t /*layout_version*/,
                                                          std::size_t /*size*/) {
    static_cast<void>(intent_.get());
    return std::unexpected{std::make_error_code(std::errc::function_not_supported)};
}

std::expected<Region, MapFailure> Region::map(int /*fd*/, SegmentKind /*kind*/,
                                              std::uint32_t /*layout_version*/, Access /*access*/) {
    return std::unexpected{
        MapFailure{.error = MapError::System,
                   .system = std::make_error_code(std::errc::function_not_supported)}};
}

} // namespace ks::ipc
