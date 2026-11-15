// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/segments/spec.md: the runtime directory and lock
// files.

#include <ks/ipc/runtime_dir.h>

#include <ks/ipc/fd.h>

#include "segments_support.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using ks::ipc::default_runtime_directory_path;
using ks::ipc::Fd;
using ks::ipc::LockFile;
using ks::ipc::LockState;
using ks::ipc::open_runtime_directory;
using ks::ipc::probe_lock;
using ks::ipc::runtime_directory_path;
using ks::ipc::test::Child;
using ks::ipc::test::TempDir;

constexpr std::string_view kName = "engine";

/// The permission bits of `path`, without following a symbolic link.
unsigned mode_of(const std::filesystem::path& path) {
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0) {
        return 0;
    }
    return static_cast<unsigned>(status.st_mode) & 07777U;
}

/// Replaces the contents of `path` with `text`.
void write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << text;
}

/// Forks a child that acquires `<dir>/<name>.lock` and holds it until it's
/// killed. Returns once the child holds the lock, or with the child gone if
/// it couldn't.
pid_t fork_holder(const std::filesystem::path& dir, std::string_view name) {
    std::array<int, 2> ends{-1, -1};
    if (::pipe(ends.data()) != 0) {
        return -1;
    }
    Fd read_end{ends[0]};
    Fd write_end{ends[1]};
    const pid_t pid = ::fork();
    if (pid == 0) {
        read_end.reset();
        const std::expected<LockFile, LockFile::Failure> lock = LockFile::acquire(dir, name);
        if (!lock) {
            ::_exit(1);
        }
        const char ready = 'r';
        if (::write(write_end.get(), &ready, 1) != 1) {
            ::_exit(2);
        }
        for (;;) {
            ::pause();
        }
    }
    write_end.reset();
    // One byte if the child holds the lock; end of file if it gave up.
    char ready = 0;
    if (pid > 0 && ::read(read_end.get(), &ready, 1) != 1) {
        return -pid;
    }
    return pid;
}

} // namespace

TEST_CASE("Runtime directory: the path", "[runtime-dir]") {
    REQUIRE(runtime_directory_path("/run/user/1000") == "/run/user/1000/kool_spin");
    REQUIRE(runtime_directory_path("/var/tmp/x/") == "/var/tmp/x/kool_spin");

#ifdef __APPLE__
    std::array<char, 1024> temporary{};
    const std::size_t length =
        ::confstr(_CS_DARWIN_USER_TEMP_DIR, temporary.data(), temporary.size());
    REQUIRE(length > 1);
    REQUIRE(length <= temporary.size());
    const std::filesystem::path fallback = std::filesystem::path{temporary.data()} / "kool_spin";
#else
    const std::filesystem::path fallback = std::format("/tmp/kool_spin-{}", ::geteuid());
#endif
    REQUIRE(runtime_directory_path(std::nullopt) == fallback);
    REQUIRE(runtime_directory_path("") == fallback);
    REQUIRE(runtime_directory_path("relative/dir") == fallback);

    // The default is one of the two forms, whatever this environment has.
    const std::filesystem::path chosen = default_runtime_directory_path();
    REQUIRE((chosen == fallback || chosen.filename() == "kool_spin"));
}

TEST_CASE("Runtime directory: created private, refused otherwise", "[runtime-dir]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());

    const std::filesystem::path created = dir.path() / "run";
    REQUIRE(open_runtime_directory(created).has_value());
    REQUIRE(std::filesystem::is_directory(created));
    REQUIRE(mode_of(created) == 0700U);
    REQUIRE(open_runtime_directory(created).has_value());

    const std::filesystem::path open_to_all = dir.path() / "wide";
    REQUIRE(::mkdir(open_to_all.c_str(), 0700) == 0);
    REQUIRE(::chmod(open_to_all.c_str(), 0755) == 0);
    REQUIRE_FALSE(open_runtime_directory(open_to_all).has_value());
    REQUIRE(mode_of(open_to_all) == 0755U);

    const std::filesystem::path link = dir.path() / "link";
    std::filesystem::create_directory_symlink(created, link);
    REQUIRE_FALSE(open_runtime_directory(link).has_value());
    REQUIRE(std::filesystem::is_symlink(link));

    const std::filesystem::path file = dir.path() / "file";
    write_file(file, "not a directory");
    REQUIRE_FALSE(open_runtime_directory(file).has_value());
    REQUIRE(std::filesystem::is_regular_file(file));
}

TEST_CASE("Lock files: one holder at a time", "[lock-files]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());

    const std::expected<LockState, std::error_code> never = probe_lock(dir.path(), "never");
    REQUIRE(never.has_value());
    REQUIRE_FALSE(never->held);

    const pid_t pid = fork_holder(dir.path(), kName);
    const Child child{pid > 0 ? pid : -pid};
    REQUIRE(pid > 0);

    const std::expected<LockFile, LockFile::Failure> second = LockFile::acquire(dir.path(), kName);
    REQUIRE_FALSE(second.has_value());
    REQUIRE(second.error().error == std::errc::resource_unavailable_try_again);
    REQUIRE(second.error().holder == pid);

    const std::expected<LockState, std::error_code> state = probe_lock(dir.path(), kName);
    REQUIRE(state.has_value());
    REQUIRE(state->held);
    REQUIRE(state->pid == pid);
}

TEST_CASE("Lock files: released when the holder dies", "[lock-files]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());

    const pid_t pid = fork_holder(dir.path(), kName);
    Child child{pid > 0 ? pid : -pid};
    REQUIRE(pid > 0);
    child.kill();

    const std::expected<LockState, std::error_code> after = probe_lock(dir.path(), kName);
    REQUIRE(after.has_value());
    REQUIRE_FALSE(after->held);

    const std::expected<LockFile, LockFile::Failure> mine = LockFile::acquire(dir.path(), kName);
    REQUIRE(mine.has_value());
    const std::expected<LockState, std::error_code> held = probe_lock(dir.path(), kName);
    REQUIRE(held.has_value());
    REQUIRE(held->held);
    REQUIRE(held->pid == ::getpid());
}

TEST_CASE("Lock files: contents and names are checked", "[lock-files]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());

    const pid_t pid = fork_holder(dir.path(), kName);
    const Child child{pid > 0 ? pid : -pid};
    REQUIRE(pid > 0);
    const std::filesystem::path file = dir.path() / "engine.lock";

    // Whatever the file says, the lock is still held: only the ID is lost.
    for (const std::string_view contents :
         {"", "abc\n", "0\n", "-5\n", "99999999999999999999\n", "123\n456\n", "12 \n", "\n"}) {
        CAPTURE(contents);
        write_file(file, contents);
        const std::expected<LockState, std::error_code> state = probe_lock(dir.path(), kName);
        REQUIRE(state.has_value());
        REQUIRE(state->held);
        REQUIRE(state->pid == std::nullopt);
    }
    for (const std::string_view contents : {"123", "123\n"}) {
        CAPTURE(contents);
        write_file(file, contents);
        const std::expected<LockState, std::error_code> state = probe_lock(dir.path(), kName);
        REQUIRE(state.has_value());
        REQUIRE(state->held);
        REQUIRE(state->pid == 123);
    }

    const std::string too_long(ks::ipc::kMaxLockNameLength + 1, 'a');
    for (const std::string_view name :
         {std::string_view{"../x"}, std::string_view{"a/b"}, std::string_view{},
          std::string_view{too_long}, std::string_view{"Engine"}}) {
        CAPTURE(name);
        const std::expected<LockFile, LockFile::Failure> lock = LockFile::acquire(dir.path(), name);
        REQUIRE_FALSE(lock.has_value());
        REQUIRE(lock.error().error == std::errc::invalid_argument);
        const std::expected<LockState, std::error_code> state = probe_lock(dir.path(), name);
        REQUIRE_FALSE(state.has_value());
        REQUIRE(state.error() == std::errc::invalid_argument);
    }
    const std::string longest(ks::ipc::kMaxLockNameLength, 'a');
    REQUIRE(LockFile::acquire(dir.path(), longest).has_value());
}
