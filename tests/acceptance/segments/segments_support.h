// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the segments acceptance tests: forked children, temporary
// runtime directories, and counting open descriptors.

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace ks::ipc::test {

/// A forked child. If a test fails while the child is still running, the
/// child is killed rather than left behind.
class Child {
public:
    explicit Child(pid_t pid) noexcept : pid_{pid} {}
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&&) = delete;
    Child& operator=(Child&&) = delete;

    ~Child() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            static_cast<void>(wait());
        }
    }

    [[nodiscard]] pid_t pid() const noexcept { return pid_; }

    /// Waits for the child to finish. Returns its exit code, or -1 if it
    /// didn't exit normally (for example, a signal killed it).
    int wait() noexcept {
        int status = 0;
        pid_t result = ::waitpid(pid_, &status, 0);
        while (result == -1 && errno == EINTR) {
            result = ::waitpid(pid_, &status, 0);
        }
        pid_ = 0;
        if (result == -1 || !WIFEXITED(status)) {
            return -1;
        }
        return WEXITSTATUS(status);
    }

    /// Kills the child with SIGKILL and waits for it.
    void kill() noexcept {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            static_cast<void>(wait());
        }
    }

private:
    pid_t pid_;
};

/// A private temporary directory (mode 0700), removed with everything in it
/// when destroyed. Each test gets its own, so tests can run in parallel.
class TempDir {
public:
    TempDir() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "ks-segments-XXXXXX").string();
        if (::mkdtemp(pattern.data()) != nullptr) {
            path_ = pattern;
        }
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    ~TempDir() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    /// The directory, or an empty path if it couldn't be created.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

/// How many of descriptors 0 to 4095 are open.
inline int open_fd_count() noexcept {
    int count = 0;
    for (int fd = 0; fd < 4096; ++fd) {
        struct stat status{};
        if (::fstat(fd, &status) == 0) {
            ++count;
        }
    }
    return count;
}

/// True if `fd` is open and close-on-exec.
inline bool is_cloexec(int fd) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX declares fcntl() variadic
    const int flags = ::fcntl(fd, F_GETFD);
    return flags != -1 && (static_cast<unsigned>(flags) & FD_CLOEXEC) != 0;
}

/// The page size.
inline std::size_t page_size() noexcept {
    return static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
}

} // namespace ks::ipc::test
