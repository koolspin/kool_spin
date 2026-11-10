// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Single-producer, single-consumer ring in shared memory (ADR 0003 §4).
///
/// The ring itself, SpscRing, is plain data in shared memory. Each side
/// attaches its own handle: a RingProducer in the process that sends, and a
/// RingConsumer in the one that receives. Each handle keeps its counter in
/// private memory and only ever writes it to the ring, so a peer that
/// scribbles on shared memory can't move it. A handle that finds the counters
/// inconsistent marks the ring corrupt and stops using it.

#include <ks/core/realtime.h>
#include <ks/ipc/layout.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace ks::ipc {

/// The shared part of a ring. Zero-filled memory is an empty ring.
///
/// `T` should hold only integers and arrays of them: its bytes come from
/// another process, and the receiver validates them after copying them out.
template <typename T, std::size_t Capacity>
struct SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "ring slots are copied as bytes");
    static_assert(std::has_single_bit(Capacity), "the capacity must be a power of two");

    static constexpr std::size_t kCapacity = Capacity;

    /// Items pushed so far. Written only by the producer.
    alignas(kSharedAlignment) std::uint64_t head;
    /// Items popped so far. Written only by the consumer.
    alignas(kSharedAlignment) std::uint64_t tail;
    /// Item n is in slots[n % Capacity].
    alignas(kSharedAlignment) std::array<T, Capacity> slots;
};

/// The sending side of a ring.
template <typename T, std::size_t Capacity>
class RingProducer {
public:
    /// Attaches to `ring` and continues from its head. The ring must outlive
    /// the producer. If the ring's counters are already inconsistent, the
    /// producer starts out corrupt.
    explicit RingProducer(SpscRing<T, Capacity>& ring) noexcept;

    RingProducer(const RingProducer&) = delete;
    RingProducer& operator=(const RingProducer&) = delete;
    RingProducer(RingProducer&&) noexcept = default;
    RingProducer& operator=(RingProducer&&) noexcept = default;
    ~RingProducer() = default;

    /// Copies `value` into the next slot and publishes it. Returns false, and
    /// changes nothing, if the ring is full or corrupt.
    [[nodiscard]] bool try_push(const T& value) noexcept KS_NONBLOCKING;

    /// True once this producer has found the ring's counters inconsistent.
    /// From then on it never touches the ring again.
    [[nodiscard]] bool corrupt() const noexcept KS_NONBLOCKING;

private:
    SpscRing<T, Capacity>* ring_;
    std::uint64_t head_ = 0; ///< Authoritative; only ever written to the ring.
    std::uint64_t tail_ = 0; ///< The tail as last read from the ring.
    bool corrupt_ = false;
};

/// The receiving side of a ring.
template <typename T, std::size_t Capacity>
class RingConsumer {
public:
    /// Attaches to `ring` and continues from its tail, as a restarted engine
    /// does. The ring must outlive the consumer. If the ring's counters are
    /// already inconsistent, the consumer starts out corrupt.
    explicit RingConsumer(SpscRing<T, Capacity>& ring) noexcept;

    RingConsumer(const RingConsumer&) = delete;
    RingConsumer& operator=(const RingConsumer&) = delete;
    RingConsumer(RingConsumer&&) noexcept = default;
    RingConsumer& operator=(RingConsumer&&) noexcept = default;
    ~RingConsumer() = default;

    /// Removes the oldest item and returns a copy of it, or nothing if the
    /// ring is empty or corrupt.
    [[nodiscard]] std::optional<T> try_pop() noexcept KS_NONBLOCKING;

    /// True once this consumer has found the ring's counters inconsistent.
    /// From then on it never touches the ring again.
    [[nodiscard]] bool corrupt() const noexcept KS_NONBLOCKING;

private:
    SpscRing<T, Capacity>* ring_;
    std::uint64_t tail_ = 0; ///< Authoritative; only ever written to the ring.
    bool corrupt_ = false;
};

// ---- Stubs ------------------------------------------------------------------
// The acceptance tests compile against this API before it's implemented
// (specs/shm-primitives/spec.md). These stubs make every test fail.

template <typename T, std::size_t Capacity>
RingProducer<T, Capacity>::RingProducer(SpscRing<T, Capacity>& ring) noexcept : ring_{&ring} {}

template <typename T, std::size_t Capacity>
bool RingProducer<T, Capacity>::try_push(const T& /*value*/) noexcept KS_NONBLOCKING {
    return ring_ == nullptr && head_ == tail_;
}

template <typename T, std::size_t Capacity>
bool RingProducer<T, Capacity>::corrupt() const noexcept KS_NONBLOCKING {
    return corrupt_;
}

template <typename T, std::size_t Capacity>
RingConsumer<T, Capacity>::RingConsumer(SpscRing<T, Capacity>& ring) noexcept : ring_{&ring} {}

template <typename T, std::size_t Capacity>
std::optional<T> RingConsumer<T, Capacity>::try_pop() noexcept KS_NONBLOCKING {
    if (ring_ == nullptr) {
        ++tail_;
    }
    return std::nullopt;
}

template <typename T, std::size_t Capacity>
bool RingConsumer<T, Capacity>::corrupt() const noexcept KS_NONBLOCKING {
    return corrupt_;
}

} // namespace ks::ipc
