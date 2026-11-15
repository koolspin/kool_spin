// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the socket protocol's acceptance tests: bytes, files with known
// identities, non-blocking sockets, and moving frames between connections.

#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/frame.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ks::ipc::test {

/// Bytes with the given values.
inline std::vector<std::byte> bytes(std::initializer_list<int> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const int value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

/// `size` bytes counting up from `seed`, wrapping at 256.
inline std::vector<std::byte> pattern(std::size_t size, unsigned seed) {
    std::vector<std::byte> result(size);
    for (std::size_t i = 0; i < size; ++i) {
        result.at(i) = static_cast<std::byte>((seed + i) & 0xFFU);
    }
    return result;
}

/// A file's identity: its device and inode.
struct FileId {
    dev_t device = 0;
    ino_t inode = 0;

    friend bool operator==(const FileId&, const FileId&) = default;
};

/// The identity of the file `fd` refers to, or nothing if fstat() fails.
inline std::optional<FileId> file_id(int fd) {
    struct stat status{};
    if (::fstat(fd, &status) != 0) {
        return std::nullopt;
    }
    return FileId{.device = status.st_dev, .inode = status.st_ino};
}

/// `count` new regular files in `dir`, opened read-only. They're different
/// files, so their identities differ. Empty if any couldn't be created.
inline std::vector<Fd> open_files(const std::filesystem::path& dir, std::size_t count) {
    std::vector<Fd> files;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string path = (dir / ("file-" + std::to_string(i))).string();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open() is variadic
        Fd file{::open(path.c_str(), O_RDONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
        if (!file.valid()) {
            return {};
        }
        files.push_back(std::move(file));
    }
    return files;
}

/// The descriptors `fds` own.
inline std::vector<int> raw(const std::vector<Fd>& fds) {
    std::vector<int> result;
    result.reserve(fds.size());
    for (const Fd& fd : fds) {
        result.push_back(fd.get());
    }
    return result;
}

/// Makes `fd` non-blocking. False if it fails.
inline bool set_nonblocking(int fd) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl() is variadic
    const int flags = ::fcntl(fd, F_GETFL);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl() is variadic
    return flags != -1 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

/// Shrinks `fd`'s send and receive buffers to about `size` bytes. False if
/// it fails.
inline bool set_buffer_sizes(int fd, int size) noexcept {
    return ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof size) == 0 &&
           ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof size) == 0;
}

/// Waits up to `timeout_ms` for `fd` to be readable. False on a timeout.
inline bool wait_readable(int fd, int timeout_ms) noexcept {
    pollfd entry{.fd = fd, .events = POLLIN, .revents = 0};
    int ready = ::poll(&entry, 1, timeout_ms);
    while (ready == -1 && errno == EINTR) {
        ready = ::poll(&entry, 1, timeout_ms);
    }
    return ready == 1;
}

/// Receives one frame, waiting up to 5 seconds for it on a non-blocking
/// connection. Nothing if none arrived in time.
inline std::expected<std::optional<ReceivedFrame>, ConnectionFailure>
receive_within(Connection& connection) {
    constexpr int kTries = 50;
    for (int i = 0; i < kTries; ++i) {
        std::expected<std::optional<ReceivedFrame>, ConnectionFailure> result =
            connection.receive();
        if (!result || *result) {
            return result;
        }
        static_cast<void>(wait_readable(connection.socket(), 100));
    }
    return std::optional<ReceivedFrame>{};
}

/// What exchange() saw.
struct Exchanged {
    std::vector<ReceivedFrame> frames;
    bool saw_queued = false; ///< has_queued() was true after a flush().
    bool failed = false;     ///< A flush() or receive() failed.
};

/// Alternates `sender.flush()` and `receiver.receive()`, in one thread, until
/// `count` frames have arrived. Both sockets should be non-blocking.
inline Exchanged exchange(Connection& sender, Connection& receiver, std::size_t count) {
    constexpr int kMaxRounds = 100'000;
    Exchanged result;
    for (int round = 0; round < kMaxRounds && result.frames.size() < count; ++round) {
        if (!sender.flush()) {
            result.failed = true;
            return result;
        }
        if (sender.has_queued()) {
            result.saw_queued = true;
        }
        for (;;) {
            std::expected<std::optional<ReceivedFrame>, ConnectionFailure> received =
                receiver.receive();
            if (!received) {
                result.failed = true;
                return result;
            }
            if (!*received) {
                break;
            }
            result.frames.push_back(std::move(**received));
        }
    }
    return result;
}

/// The address of the Unix socket at `path`. False if it doesn't fit.
inline bool socket_address(const std::filesystem::path& path, sockaddr_un& address) noexcept {
    address = {};
    address.sun_family = AF_UNIX;
    const std::string& native = path.native();
    if (native.size() + 1 > sizeof address.sun_path) {
        return false;
    }
    std::memcpy(static_cast<void*>(address.sun_path), native.c_str(), native.size() + 1);
    return true;
}

/// Leaves a socket file at `path`, as a process that died would: binds a Unix
/// socket there, then closes it without unlinking the file. False if it fails.
inline bool leave_socket_file(const std::filesystem::path& path) {
    sockaddr_un address{};
    if (!socket_address(path, address)) {
        return false;
    }
    const Fd socket{::socket(AF_UNIX, SOCK_STREAM, 0)};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
    const auto* const generic = reinterpret_cast<const sockaddr*>(&address);
    return socket.valid() && ::bind(socket.get(), generic, sizeof address) == 0;
}

/// The type and permission bits of `path`, without following a symbolic
/// link, or 0 if it doesn't exist.
inline mode_t mode_of(const std::filesystem::path& path) {
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0) {
        return 0;
    }
    return status.st_mode;
}

} // namespace ks::ipc::test
