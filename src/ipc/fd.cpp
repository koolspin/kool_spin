// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/fd.h>

#include <unistd.h>

#include <utility>

namespace ks::ipc {

Fd::Fd(Fd&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}

Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) {
        reset(std::exchange(other.fd_, -1));
    }
    return *this;
}

Fd::~Fd() {
    reset();
}

int Fd::release() noexcept {
    return std::exchange(fd_, -1);
}

void Fd::reset(int fd) noexcept {
    const int old = std::exchange(fd_, fd < 0 ? -1 : fd);
    if (old >= 0) {
        // Not retried on EINTR: on Linux and macOS the descriptor is closed
        // even then, and retrying could close a descriptor another thread
        // has just been given.
        static_cast<void>(::close(old));
    }
}

} // namespace ks::ipc
