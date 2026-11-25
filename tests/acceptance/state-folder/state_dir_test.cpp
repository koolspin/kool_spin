// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/state-folder/spec.md: where the state folder is,
// and which folders are refused (requirements 1 to 4).

#include <ks/ipc/state_dir.h>

#include "segments_support.h"
#include <catch2/catch_test_macros.hpp>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

using ks::ipc::choose_state_directory;
using ks::ipc::default_state_directory_path;
using ks::ipc::FileSystemVerdict;
using ks::ipc::judge_file_system;
using ks::ipc::open_state_directory;
using ks::ipc::state_directory_path;
using ks::ipc::StateDirectoryChoice;
using ks::ipc::StateDirectoryFailure;
using ks::ipc::StateDirectorySource;
using ks::ipc::test::Child;
using ks::ipc::test::TempDir;

/// The state folder under the home directory `home`, as requirement 1 says.
fs::path under_home(const fs::path& home) {
#ifdef __APPLE__
    return home / "Library/Application Support/kool_spin";
#else
    return home / ".local/state/kool_spin";
#endif
}

bool is_absolute(const std::optional<std::string_view>& value) {
    return value && value->starts_with('/');
}

/// The permission bits of `path`, which must exist.
unsigned mode_of(const fs::path& path) {
    struct stat status{};
    REQUIRE(::lstat(path.c_str(), &status) == 0);
    return static_cast<unsigned>(status.st_mode) & 0777U;
}

void set_mode(const fs::path& path, mode_t mode) {
    REQUIRE(::chmod(path.c_str(), mode) == 0);
}

/// Why `path` is refused, or an empty error if it isn't.
std::error_code refusal(const fs::path& path) {
    const std::expected<void, StateDirectoryFailure> opened = open_state_directory(path, false);
    return opened ? std::error_code{} : opened.error().error;
}

/// The effective user's home directory in the user database; empty if there
/// isn't one.
std::string account_home() {
    std::vector<char> buffer(std::size_t{64} * 1024);
    struct passwd entry{};
    struct passwd* found = nullptr; // NOLINT(misc-const-correctness): getpwuid_r() sets it
    if (::getpwuid_r(::geteuid(), &entry, buffer.data(), buffer.size(), &found) != 0 ||
        found == nullptr || found->pw_dir == nullptr) {
        return {};
    }
    return std::string{found->pw_dir};
}

/// True if default_state_directory_path() is `expected` in a process whose
/// HOME and XDG_STATE_HOME are `home` and `xdg`, or unset. The child changes
/// its own environment, never this process's.
bool default_is(const std::optional<fs::path>& expected, const char* home, const char* xdg) {
    const pid_t pid = ::fork();
    REQUIRE(pid != -1);
    if (pid == 0) {
        const auto set = [](const char* name, const char* value) {
            // NOLINTBEGIN(concurrency-mt-unsafe): the child has one thread
            return value != nullptr ? ::setenv(name, value, 1) : ::unsetenv(name);
            // NOLINTEND(concurrency-mt-unsafe)
        };
        const bool ready = set("HOME", home) == 0 && set("XDG_STATE_HOME", xdg) == 0;
        ::_exit(ready && default_state_directory_path() == expected ? 0 : 1);
    }
    Child child{pid};
    return child.wait() == 0;
}

} // namespace

TEST_CASE("State folder: the path", "[state-folder]") {
    SECTION("every combination") {
        const auto values = [](std::string_view absolute) {
            return std::array<std::optional<std::string_view>, 4>{
                std::nullopt, std::string_view{}, std::string_view{"relative/path"}, absolute};
        };
        for (const auto& home : values("/home/dj")) {
            for (const auto& xdg : values("/var/xdg state")) {
                for (const auto& account : values("/accounts/dj")) {
                    std::optional<fs::path> expected;
                    if (is_absolute(account)) {
                        expected = under_home("/accounts/dj");
                    }
                    if (is_absolute(home)) {
                        expected = under_home("/home/dj");
                    }
#ifndef __APPLE__
                    if (is_absolute(xdg)) {
                        expected = "/var/xdg state/kool_spin";
                    }
#endif
                    CAPTURE(home.value_or("missing"), xdg.value_or("missing"),
                            account.value_or("missing"));
                    CHECK(state_directory_path(home, xdg, account) == expected);
                }
            }
        }
    }

    SECTION("the default") {
        CHECK(default_is(under_home("/home/dj"), "/home/dj", nullptr));
        CHECK(default_is(under_home("/home/dj"), "/home/dj", "relative"));
#ifdef __APPLE__
        CHECK(default_is(under_home("/home/dj"), "/home/dj", "/xdg"));
#else
        CHECK(default_is("/xdg/kool_spin", "/home/dj", "/xdg"));
#endif
        // Without an environment, as under cron.
        const std::string account = account_home();
        if (account.starts_with('/')) {
            CHECK(default_is(under_home(account), nullptr, nullptr));
            CHECK(default_is(under_home(account), "", ""));
        }
    }
}

TEST_CASE("State folder: which one, and whether it was named", "[state-folder]") {
    const std::optional<fs::path> standard = default_state_directory_path();
    REQUIRE(standard.has_value());

    for (const bool has_runtime_dir : {false, true}) {
        CAPTURE(has_runtime_dir);
        const StateDirectoryChoice other = choose_state_directory("/elsewhere", has_runtime_dir);
        CHECK(other.source == StateDirectorySource::Named);
        CHECK(other.path == fs::path{"/elsewhere"});
        // Naming the default's path is naming it.
        const StateDirectoryChoice same = choose_state_directory(standard, has_runtime_dir);
        CHECK(same.source == StateDirectorySource::Named);
        CHECK(same.path == standard);
    }

    const StateDirectoryChoice chosen = choose_state_directory(std::nullopt, false);
    CHECK(chosen.source == StateDirectorySource::Default);
    CHECK(chosen.path == standard);

    const StateDirectoryChoice none = choose_state_directory(std::nullopt, true);
    CHECK(none.source == StateDirectorySource::None);
    CHECK(!none.path.has_value());
}

TEST_CASE("State folder: created private, refused otherwise", "[state-folder]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());

    SECTION("missing parents") {
        const fs::path parent = dir.path() / "parent";
        REQUIRE(fs::create_directory(parent));
        set_mode(parent, 0755);
        const fs::path folder = parent / "a" / "b" / "kool_spin";
        CHECK(refusal(folder) == std::error_code{});
        REQUIRE(fs::is_directory(folder));
        CHECK(mode_of(folder) == 0700U);
        CHECK(mode_of(folder.parent_path()) == 0700U);
        CHECK(mode_of(folder.parent_path().parent_path()) == 0700U);
        // What was there isn't changed.
        CHECK(mode_of(parent) == 0755U);
        // Again, now that it's there.
        CHECK(refusal(folder) == std::error_code{});
    }

    SECTION("a file") {
        const fs::path file = dir.path() / "file";
        std::ofstream{file} << "contents";
        const unsigned before = mode_of(file);
        CHECK(refusal(file) == std::errc::not_a_directory);
        CHECK(fs::is_regular_file(file));
        CHECK(fs::file_size(file) == 8);
        CHECK(mode_of(file) == before);
        // And as a parent.
        CHECK(refusal(file / "kool_spin") != std::error_code{});
        CHECK(fs::is_regular_file(file));
    }

    SECTION("modes") {
        const fs::path folder = dir.path() / "folder";
        REQUIRE(fs::create_directory(folder));
        for (const mode_t mode : std::array<mode_t, 5>{0720, 0770, 0702, 0707, 0777}) {
            CAPTURE(mode);
            set_mode(folder, mode);
            CHECK(refusal(folder) == std::errc::permission_denied);
            CHECK(mode_of(folder) == static_cast<unsigned>(mode));
        }
        for (const mode_t mode : std::array<mode_t, 4>{0700, 0750, 0755, 0500}) {
            CAPTURE(mode);
            set_mode(folder, mode);
            CHECK(refusal(folder) == std::error_code{});
            CHECK(mode_of(folder) == static_cast<unsigned>(mode));
        }
    }

    SECTION("links") {
        const fs::path target = dir.path() / "target";
        const fs::path link = dir.path() / "link";
        fs::create_symlink(target, link);
        // Dangling: it isn't followed to create something elsewhere.
        CHECK(refusal(link) != std::error_code{});
        CHECK(fs::is_symlink(link));
        CHECK(!fs::exists(target));

        std::ofstream{target} << "contents";
        CHECK(refusal(link) == std::errc::not_a_directory);
        CHECK(fs::file_size(target) == 8);

        REQUIRE(fs::remove(target));
        REQUIRE(fs::create_directory(target));
        set_mode(target, 0700);
        CHECK(refusal(link) == std::error_code{});
        CHECK(fs::is_symlink(link));
        // It's the folder's own mode that counts.
        set_mode(target, 0770);
        CHECK(refusal(link) == std::errc::permission_denied);
        CHECK(mode_of(target) == 0770U);
    }

    SECTION("another user's") {
        if (::geteuid() != 0) {
            CHECK(refusal("/usr") == std::errc::permission_denied);
        }
    }
}

TEST_CASE("State folder: network file systems and FUSE", "[state-folder]") {
#ifdef __APPLE__
    struct ::statfs status{};
    for (const bool named : {false, true}) {
        CAPTURE(named);
        status.f_flags = MNT_LOCAL;
        CHECK(judge_file_system(status, named) == FileSystemVerdict::Usable);
        status.f_flags = MNT_LOCAL | MNT_RDONLY | MNT_JOURNALED;
        CHECK(judge_file_system(status, named) == FileSystemVerdict::Usable);
        status.f_flags = 0;
        CHECK(judge_file_system(status, named) == FileSystemVerdict::Network);
        status.f_flags = MNT_RDONLY;
        CHECK(judge_file_system(status, named) == FileSystemVerdict::Network);
    }
#else
    const auto judge = [](std::uint32_t type, bool named) {
        struct ::statfs status{};
        status.f_type = static_cast<decltype(status.f_type)>(type);
        return judge_file_system(status, named);
    };
    // As <linux/magic.h> and the kernel's sources give them.
    constexpr std::array<std::uint32_t, 4> kLocal{
        0xEF53,     // ext4
        0x9123683E, // btrfs
        0x58465342, // XFS
        0x01021994, // tmpfs
    };
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
    for (const std::uint32_t type : kLocal) {
        CAPTURE(type);
        CHECK(judge(type, false) == FileSystemVerdict::Usable);
        CHECK(judge(type, true) == FileSystemVerdict::Usable);
    }
    for (const std::uint32_t type : kNetwork) {
        CAPTURE(type);
        CHECK(judge(type, false) == FileSystemVerdict::Network);
        CHECK(judge(type, true) == FileSystemVerdict::Network);
    }
    CHECK(judge(kFuse, false) == FileSystemVerdict::Fuse);
    CHECK(judge(kFuse, true) == FileSystemVerdict::Usable);
#endif
}
