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
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
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

// ---- Implementation -----------------------------------------------------------
//
// Why the fences are where they are (Boehm, "Can seqlocks get along with
// programming language memory models?", 2012):
// - If a reader's relaxed load sees a payload word from a write, the writer's
//   release fence and the reader's acquire fence synchronize. The writer's odd
//   store, which comes before its fence, then happens before the reader's
//   recheck, so the recheck can't still see the even number from before.
// - If the reader's first load sees the even number a write ended with, the
//   writer's final release store synchronizes with that acquire load, so the
//   payload loads see that write's words or later ones.

template <typename T>
SeqlockWriter<T>::SeqlockWriter(Seqlock<T>& lock) noexcept
    : lock_{&lock},
      sequence_{(load_shared(lock.sequence, std::memory_order_relaxed) + 1) & ~std::uint64_t{1}} {}

template <typename T>
void SeqlockWriter<T>::write(const T& value) noexcept KS_NONBLOCKING {
    std::array<std::uint64_t, Seqlock<T>::kWords> words{};
    std::memcpy(words.data(), &value, sizeof(T));
    const std::span<const std::uint64_t, Seqlock<T>::kWords> local{words};
    const std::span<std::uint64_t, Seqlock<T>::kWords> shared{lock_->words};

    store_shared(lock_->sequence, sequence_ + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (std::size_t i = 0; i < Seqlock<T>::kWords; ++i) {
        store_shared(shared[i], local[i], std::memory_order_relaxed);
    }
    sequence_ += 2;
    store_shared(lock_->sequence, sequence_, std::memory_order_release);
}

template <typename T>
SeqlockReader<T>::SeqlockReader(Seqlock<T>& lock) noexcept : lock_{&lock} {}

template <typename T>
std::optional<T>
SeqlockReader<T>::try_read(std::size_t max_attempts) const noexcept KS_NONBLOCKING {
    std::array<std::uint64_t, Seqlock<T>::kWords> words{};
    const std::span<std::uint64_t, Seqlock<T>::kWords> local{words};
    const std::span<std::uint64_t, Seqlock<T>::kWords> shared{lock_->words};

    for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
        const std::uint64_t before = load_shared(lock_->sequence, std::memory_order_acquire);
        if (before % 2 != 0) {
            continue; // a write is in progress
        }
        for (std::size_t i = 0; i < Seqlock<T>::kWords; ++i) {
            local[i] = load_shared(shared[i], std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (load_shared(lock_->sequence, std::memory_order_relaxed) == before) {
            std::array<std::byte, sizeof(T)> bytes{};
            std::memcpy(bytes.data(), words.data(), sizeof(T));
            return std::bit_cast<T>(bytes);
        }
    }
    return std::nullopt;
}

} // namespace ks::ipc
