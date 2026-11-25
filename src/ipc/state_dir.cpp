// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/state_dir.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

// Stubs: the state folder (specs/state-folder/spec.md) isn't implemented yet.

namespace ks::ipc {

std::optional<std::filesystem::path>
state_directory_path(std::optional<std::string_view> /*home*/,
                     std::optional<std::string_view> /*xdg_state_home*/,
                     std::optional<std::string_view> /*account_home*/) {
    return std::nullopt;
}

std::optional<std::filesystem::path> default_state_directory_path() {
    return std::nullopt;
}

StateDirectoryChoice
choose_state_directory(const std::optional<std::filesystem::path>& /*state_dir*/,
                       bool /*has_runtime_dir*/) {
    return {};
}

FileSystemVerdict judge_file_system(const struct ::statfs& /*status*/, bool /*named*/) noexcept {
    return FileSystemVerdict::Usable;
}

std::expected<void, StateDirectoryFailure>
open_state_directory(const std::filesystem::path& /*path*/, bool /*named*/) {
    return std::unexpected{StateDirectoryFailure{
        .error = std::make_error_code(std::errc::function_not_supported), .fuse = false}};
}

} // namespace ks::ipc
