// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Seqlock in shared memory: one writer, any number of readers, and nobody
/// ever waits (ADR 0003 §5).
///
/// The writer marks a write in progress by making the sequence number odd.
/// A reader copies the value and accepts the copy only if the sequence number
/// was the same even number before and after. A seqlock over plain memory is
/// a data race in C++, so the value is stored as 64-bit words accessed through
/// std::atomic_ref with relaxed ordering, and fences order them.

#include <ks/core/realtime.h>
#include <ks/ipc/layout.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace ks::ipc {

/// The shared part of a seqlock. Zero-filled memory is a seqlock holding a
/// zero value.
///
/// `T` should hold only integers and arrays of them: its bytes come from
/// another process, and the reader validates them after copying them out.
template <typename T>
struct Seqlock {
    static_assert(std::is_trivially_copyable_v<T>, "seqlock values are copied as bytes");

    static constexpr std::size_t kWords =
        (sizeof(T) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);

    /// Odd while a write is in progress.
    alignas(kSharedAlignment) std::uint64_t sequence;
    /// The value's bytes, zero-padded to whole words.
    std::array<std::uint64_t, kWords> words;
};

/// The only writer of a seqlock.
template <typename T>
class SeqlockWriter {
public:
    /// Attaches to `lock` and continues from its sequence number, rounded up
    /// to an even number if a previous writer stopped mid-write. The seqlock
    /// must outlive the writer.
    explicit SeqlockWriter(Seqlock<T>& lock) noexcept;

    SeqlockWriter(const SeqlockWriter&) = delete;
    SeqlockWriter& operator=(const SeqlockWriter&) = delete;
    SeqlockWriter(SeqlockWriter&&) noexcept = default;
    SeqlockWriter& operator=(SeqlockWriter&&) noexcept = default;
    ~SeqlockWriter() = default;

    /// Publishes `value`.
    void write(const T& value) noexcept KS_NONBLOCKING;

private:
    Seqlock<T>* lock_;
    std::uint64_t sequence_ = 0; ///< Authoritative; even between writes.
};

/// A reader of a seqlock. Any number of readers can read at once.
template <typename T>
class SeqlockReader {
public:
    static constexpr std::size_t kDefaultAttempts = 16;

    /// The seqlock must outlive the reader. It's taken by non-const
    /// reference because std::atomic_ref needs one; the reader never writes.
    explicit SeqlockReader(Seqlock<T>& lock) noexcept;

    /// Returns a consistent copy of the value, or nothing if a write was in
    /// progress during each of `max_attempts` attempts. Never waits.
    [[nodiscard]] std::optional<T>
    try_read(std::size_t max_attempts = kDefaultAttempts) const noexcept KS_NONBLOCKING;

private:
    Seqlock<T>* lock_;
};

// ---- Stubs ------------------------------------------------------------------
// The acceptance tests compile against this API before it's implemented
// (specs/shm-primitives/spec.md). These stubs make every test fail.

template <typename T>
SeqlockWriter<T>::SeqlockWriter(Seqlock<T>& lock) noexcept : lock_{&lock} {}

template <typename T>
void SeqlockWriter<T>::write(const T& /*value*/) noexcept KS_NONBLOCKING {
    if (lock_ == nullptr) {
        ++sequence_;
    }
}

template <typename T>
SeqlockReader<T>::SeqlockReader(Seqlock<T>& lock) noexcept : lock_{&lock} {}

template <typename T>
std::optional<T>
SeqlockReader<T>::try_read(std::size_t /*max_attempts*/) const noexcept KS_NONBLOCKING {
    if (lock_ == nullptr) {
        return T{};
    }
    return std::nullopt;
}

} // namespace ks::ipc
