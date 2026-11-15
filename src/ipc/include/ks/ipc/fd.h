// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// An owned file descriptor (specs/segments/spec.md requirement 5).

namespace ks::ipc {

/// Owns one file descriptor, and closes it when destroyed. A default Fd owns
/// nothing. Move-only.
class Fd {
public:
    Fd() noexcept = default;
    /// Takes ownership of `fd`. A negative `fd` means nothing is owned.
    explicit Fd(int fd) noexcept : fd_{fd < 0 ? -1 : fd} {}

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;
    ~Fd();

    /// The descriptor, or -1 if nothing is owned.
    [[nodiscard]] int get() const noexcept { return fd_; }
    /// True if a descriptor is owned.
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    /// Gives up the descriptor without closing it, and returns it.
    [[nodiscard]] int release() noexcept;
    /// Closes the owned descriptor, if any, and takes ownership of `fd`.
    void reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};

} // namespace ks::ipc
