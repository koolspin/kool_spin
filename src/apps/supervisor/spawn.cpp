// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/supervisor/spawn.h>

#include <ks/ipc/fd.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <csignal>

#ifdef __APPLE__
#include <crt_externs.h>
#endif

#include <algorithm>
#include <cerrno>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef __APPLE__
// The environment, for posix_spawn(). POSIX declares it nowhere.
extern "C" char** environ; // NOLINT(readability-redundant-declaration): see above
#endif

namespace ks::supervisor {

namespace {

std::error_code error_from(int code) noexcept {
    return {code, std::system_category()};
}

/// This process's environment.
char** environment() noexcept {
#ifdef __APPLE__
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

/// A copy of `fd` at `minimum` or above, close-on-exec, or an invalid Fd.
ipc::Fd duplicate_above(int fd, int minimum) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl() is variadic
    return ipc::Fd{::fcntl(fd, F_DUPFD_CLOEXEC, minimum)};
}

/// posix_spawn's file actions, destroyed with the object.
class FileActions {
public:
    FileActions() noexcept : error_{::posix_spawn_file_actions_init(&actions_)} {}
    FileActions(const FileActions&) = delete;
    FileActions& operator=(const FileActions&) = delete;
    FileActions(FileActions&&) = delete;
    FileActions& operator=(FileActions&&) = delete;
    ~FileActions() {
        if (error_ == 0) {
            static_cast<void>(::posix_spawn_file_actions_destroy(&actions_));
        }
    }

    [[nodiscard]] int error() const noexcept { return error_; }
    [[nodiscard]] posix_spawn_file_actions_t* get() noexcept { return &actions_; }

private:
    posix_spawn_file_actions_t actions_{};
    int error_ = 0;
};

/// posix_spawn's attributes, destroyed with the object.
class Attributes {
public:
    Attributes() noexcept : error_{::posix_spawnattr_init(&attributes_)} {}
    Attributes(const Attributes&) = delete;
    Attributes& operator=(const Attributes&) = delete;
    Attributes(Attributes&&) = delete;
    Attributes& operator=(Attributes&&) = delete;
    ~Attributes() {
        if (error_ == 0) {
            static_cast<void>(::posix_spawnattr_destroy(&attributes_));
        }
    }

    [[nodiscard]] int error() const noexcept { return error_; }
    [[nodiscard]] posix_spawnattr_t* get() noexcept { return &attributes_; }

private:
    posix_spawnattr_t attributes_{};
    int error_ = 0;
};

/// True if every `child` number is 3 or more and given once, and every
/// `parent` is non-negative.
bool valid(std::span<const Inherit> fds) {
    std::vector<int> children;
    for (const Inherit& fd : fds) {
        if (fd.parent < 0 || fd.child < 3) {
            return false;
        }
        children.push_back(fd.child);
    }
    std::ranges::sort(children);
    return std::ranges::adjacent_find(children) == children.end();
}

/// A new process group, an empty signal mask, and every signal at its
/// default disposition. On macOS, also: close every descriptor not passed on
/// explicitly.
int set_attributes(posix_spawnattr_t* attributes) noexcept {
    sigset_t none;
    sigemptyset(&none);
    sigset_t all;
    sigfillset(&all);
#ifdef __APPLE__
    const auto flags = static_cast<short>(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
                                          POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
    const auto flags =
        static_cast<short>(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
#endif
    int error = ::posix_spawnattr_setflags(attributes, flags);
    if (error == 0) {
        error = ::posix_spawnattr_setpgroup(attributes, 0);
    }
    if (error == 0) {
        error = ::posix_spawnattr_setsigmask(attributes, &none);
    }
    if (error == 0) {
        error = ::posix_spawnattr_setsigdefault(attributes, &all);
    }
    return error;
}

/// Moves each of `sources` to its child number, and closes every other
/// descriptor from 3 up. `sources` are close-on-exec copies above every child
/// number, so no move overwrites a descriptor that a later one needs.
int set_file_actions(posix_spawn_file_actions_t* actions, std::span<const Inherit> fds,
                     std::span<const ipc::Fd> sources) noexcept {
    int highest_child = 2;
    for (std::size_t i = 0; i < fds.size(); ++i) {
        const int error =
            ::posix_spawn_file_actions_adddup2(actions, sources[i].get(), fds[i].child);
        if (error != 0) {
            return error;
        }
        highest_child = std::max(highest_child, fds[i].child);
    }
#ifdef __APPLE__
    // POSIX_SPAWN_CLOEXEC_DEFAULT closes everything else; keep 0 to 2.
    static_cast<void>(highest_child);
    for (int fd = 0; fd <= 2; ++fd) {
        const int error = ::posix_spawn_file_actions_addinherit_np(actions, fd);
        if (error != 0) {
            return error;
        }
    }
#else
    // Close the gaps below the highest child number, then everything above
    // it. A close of a descriptor that isn't open is ignored.
    for (int fd = 3; fd < highest_child; ++fd) {
        const bool given =
            std::ranges::any_of(fds, [fd](const Inherit& i) { return i.child == fd; });
        if (!given) {
            const int error = ::posix_spawn_file_actions_addclose(actions, fd);
            if (error != 0) {
                return error;
            }
        }
    }
    const int error = ::posix_spawn_file_actions_addclosefrom_np(actions, highest_child + 1);
    if (error != 0) {
        return error;
    }
#endif
    return 0;
}

} // namespace

std::expected<pid_t, std::error_code> spawn(const std::filesystem::path& program,
                                            std::span<const std::string> args,
                                            std::span<const Inherit> fds) {
    if (!valid(fds)) {
        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    }

    // Refuse a program that can't run before creating a process. On macOS, a
    // posix_spawn() whose exec fails still creates a child, which the kernel
    // then ends, so for a moment the caller has a child it never asked for.
    struct stat status{};
    if (::stat(program.c_str(), &status) != 0) {
        return std::unexpected{error_from(errno)};
    }
    if (!S_ISREG(status.st_mode)) {
        return std::unexpected{std::make_error_code(std::errc::permission_denied)};
    }
    if (::access(program.c_str(), X_OK) != 0) {
        return std::unexpected{error_from(errno)};
    }

    // Copies of the descriptors to pass on, above every child number.
    int above = 3;
    for (const Inherit& fd : fds) {
        above = std::max(above, fd.child + 1);
    }
    std::vector<ipc::Fd> sources;
    for (const Inherit& fd : fds) {
        ipc::Fd copy = duplicate_above(fd.parent, above);
        if (!copy.valid()) {
            return std::unexpected{error_from(errno)};
        }
        sources.push_back(std::move(copy));
    }

    FileActions actions;
    if (actions.error() != 0) {
        return std::unexpected{error_from(actions.error())};
    }
    if (const int error = set_file_actions(actions.get(), fds, sources); error != 0) {
        return std::unexpected{error_from(error)};
    }
    Attributes attributes;
    if (attributes.error() != 0) {
        return std::unexpected{error_from(attributes.error())};
    }
    if (const int error = set_attributes(attributes.get()); error != 0) {
        return std::unexpected{error_from(error)};
    }

    // posix_spawn() takes non-const strings: give it copies.
    std::vector<std::string> strings{program.string()};
    strings.insert(strings.end(), args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(strings.size() + 1);
    for (std::string& string : strings) {
        argv.push_back(string.data());
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int error = ::posix_spawn(&pid, strings.front().c_str(), actions.get(), attributes.get(),
                                    argv.data(), environment());
    if (error != 0) {
        return std::unexpected{error_from(error)};
    }
    return pid;
}

} // namespace ks::supervisor
