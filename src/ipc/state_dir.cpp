// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/state_dir.h>

#include "posix.h"
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ks::ipc {

namespace {

using detail::last_error;
using detail::retry_eintr;

constexpr std::string_view kDirectoryName = "kool_spin";
constexpr mode_t kDirMode = 0700;

std::unexpected<StateDirectoryFailure> failed(std::error_code error) {
    return std::unexpected{StateDirectoryFailure{.error = error}};
}

/// `value` as a path, if it's absolute.
std::optional<std::filesystem::path> absolute(std::optional<std::string_view> value) {
    if (!value || !value->starts_with('/')) {
        return std::nullopt;
    }
    return std::filesystem::path{*value};
}

/// The state folder under the home directory `home`.
std::filesystem::path under_home(const std::filesystem::path& home) {
#ifdef __APPLE__
    return home / "Library" / "Application Support" / kDirectoryName;
#else
    return home / ".local" / "state" / kDirectoryName;
#endif
}

/// The variable `name`, if it's set.
std::optional<std::string_view> variable(const char* name) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing changes the environment
    const char* const value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string_view{value};
}

/// The effective user's home directory in the user database.
std::optional<std::string> account_home() {
    // Entries are a few hundred bytes; the limit is for a database gone wrong.
    constexpr std::size_t kMaxEntry = std::size_t{1} << 20U;
    for (std::size_t size = 4'096; size <= kMaxEntry; size *= 2) {
        std::vector<char> buffer(size);
        struct passwd entry{};
        struct passwd* found = nullptr; // NOLINT(misc-const-correctness): getpwuid_r() sets it
        const int failed = ::getpwuid_r(::geteuid(), &entry, buffer.data(), buffer.size(), &found);
        if (failed == ERANGE) {
            continue;
        }
        if (failed != 0 || found == nullptr || found->pw_dir == nullptr) {
            return std::nullopt;
        }
        return std::string{found->pw_dir};
    }
    return std::nullopt;
}

} // namespace

std::optional<std::filesystem::path>
state_directory_path(std::optional<std::string_view> home,
                     [[maybe_unused]] std::optional<std::string_view> xdg_state_home,
                     std::optional<std::string_view> account_home) {
#ifndef __APPLE__
    if (const auto xdg = absolute(xdg_state_home)) {
        return *xdg / kDirectoryName;
    }
#endif
    if (const auto dir = absolute(home)) {
        return under_home(*dir);
    }
    if (const auto dir = absolute(account_home)) {
        return under_home(*dir);
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> default_state_directory_path() {
    const std::optional<std::string_view> home = variable("HOME");
    const std::optional<std::string_view> xdg = variable("XDG_STATE_HOME");
    if (std::optional<std::filesystem::path> path = state_directory_path(home, xdg, {})) {
        return path;
    }
    const std::optional<std::string> account = account_home();
    if (!account) {
        return std::nullopt;
    }
    return state_directory_path(home, xdg, *account);
}

StateDirectoryChoice choose_state_directory(const std::optional<std::filesystem::path>& state_dir,
                                            bool has_runtime_dir) {
    if (state_dir) {
        return {.source = StateDirectorySource::Named, .path = state_dir};
    }
    if (has_runtime_dir) {
        return {};
    }
    return {.source = StateDirectorySource::Default, .path = default_state_directory_path()};
}

FileSystemVerdict judge_file_system(const struct ::statfs& status,
                                    [[maybe_unused]] bool named) noexcept {
#ifdef __APPLE__
    return (status.f_flags & MNT_LOCAL) != 0 ? FileSystemVerdict::Usable
                                             : FileSystemVerdict::Network;
#else
    constexpr std::array<std::uint32_t, 9> kNetwork{
        0x6969,     // NFS
        0x517B,     // SMB
        0xFE534D42, // SMB2
        0xFF534D42, // CIFS
        0x5346414F, // AFS (OpenAFS)
        0x6B414653, // AFS (the kernel's)
        0x73757245, // Coda
        0x01021997, // 9P
        0x00C36400, // Ceph
    };
    constexpr std::uint32_t kFuse = 0x65735546;
    const auto type = static_cast<std::uint32_t>(status.f_type);
    if (std::ranges::contains(kNetwork, type)) {
        return FileSystemVerdict::Network;
    }
    return type == kFuse && !named ? FileSystemVerdict::Fuse : FileSystemVerdict::Usable;
#endif
}

std::string describe(const StateDirectoryFailure& failure) {
    if (failure.writable) {
        return "its group or others can write to it (chmod go-w makes it usable)";
    }
    if (failure.fuse) {
        return "it's on a FUSE file system, which another machine may share (if none does, name "
               "the folder with --state-dir)";
    }
    if (failure.error == std::errc::not_supported) {
        return "it's on a network file system";
    }
    return failure.error.message();
}

std::expected<void, StateDirectoryFailure> check_state_directory(const std::filesystem::path& path,
                                                                 bool named) {
    struct stat status{};
    if (::stat(path.c_str(), &status) == -1) {
        return failed(last_error());
    }
    if (!S_ISDIR(status.st_mode)) {
        return failed(std::make_error_code(std::errc::not_a_directory));
    }
    if (status.st_uid != ::geteuid()) {
        return failed(std::make_error_code(std::errc::permission_denied));
    }
    if ((status.st_mode & 0022U) != 0) {
        return std::unexpected{
            StateDirectoryFailure{.error = std::make_error_code(std::errc::permission_denied),
                                  .fuse = false,
                                  .writable = true}};
    }
    struct statfs file_system{};
    if (retry_eintr([&] { return ::statfs(path.c_str(), &file_system); }) == -1) {
        return failed(last_error());
    }
    const FileSystemVerdict verdict = judge_file_system(file_system, named);
    if (verdict == FileSystemVerdict::Usable) {
        return {};
    }
    return std::unexpected{
        StateDirectoryFailure{.error = std::make_error_code(std::errc::not_supported),
                              .fuse = verdict == FileSystemVerdict::Fuse,
                              .writable = false}};
}

std::expected<void, StateDirectoryFailure> open_state_directory(const std::filesystem::path& path,
                                                                bool named) {
    std::filesystem::path made;
    for (const std::filesystem::path& part : path) {
        made /= part;
        if (::mkdir(made.c_str(), kDirMode) == -1 && errno != EEXIST) {
            const std::error_code error = last_error();
            // Not every system says EEXIST for a directory that's there: one
            // on a read-only disk, or macOS's "/".
            struct stat status{};
            if (::stat(made.c_str(), &status) == -1 || !S_ISDIR(status.st_mode)) {
                return failed(error);
            }
        }
    }
    return check_state_directory(path, named);
}

} // namespace ks::ipc
