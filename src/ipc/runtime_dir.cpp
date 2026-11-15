// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/runtime_dir.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

namespace ks::ipc {

// Stubs, until the maintainer approves the acceptance tests for
// specs/segments/spec.md.

std::filesystem::path runtime_directory_path(std::optional<std::string_view> /*base*/) {
    return {};
}

std::filesystem::path default_runtime_directory_path() {
    return {};
}

std::expected<void, std::error_code> open_runtime_directory(const std::filesystem::path& /*path*/) {
    return std::unexpected{std::make_error_code(std::errc::function_not_supported)};
}

std::expected<LockFile, LockFile::Failure> LockFile::acquire(const std::filesystem::path& /*dir*/,
                                                             std::string_view /*name*/) {
    return std::unexpected{
        Failure{.error = std::make_error_code(std::errc::function_not_supported), .holder = {}}};
}

std::expected<LockState, std::error_code> probe_lock(const std::filesystem::path& /*dir*/,
                                                     std::string_view /*name*/) {
    return std::unexpected{std::make_error_code(std::errc::function_not_supported)};
}

} // namespace ks::ipc
