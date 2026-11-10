// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/spsc_ring.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>

namespace {

using ks::ipc::RingConsumer;
using ks::ipc::RingProducer;
using ks::ipc::SpscRing;

struct Small {
    std::array<std::uint8_t, 3> bytes;

    friend constexpr bool operator==(const Small&, const Small&) = default;
};

} // namespace

TEST_CASE("SpscRing carries slots of any size", "[ipc][ring]") {
    SpscRing<Small, 4> ring{};
    RingProducer producer{ring};
    RingConsumer consumer{ring};
    for (std::uint8_t n = 0; n < 10; ++n) {
        const Small value{.bytes = {n, static_cast<std::uint8_t>(n + 1), 0xFF}};
        REQUIRE(producer.try_push(value));
        REQUIRE(consumer.try_pop() == value);
    }
}

TEST_CASE("SpscRing with one slot alternates between full and empty", "[ipc][ring]") {
    SpscRing<std::uint64_t, 1> ring{};
    RingProducer producer{ring};
    RingConsumer consumer{ring};
    for (std::uint64_t n = 0; n < 5; ++n) {
        REQUIRE(producer.try_push(n));
        REQUIRE_FALSE(producer.try_push(n));
        REQUIRE(consumer.try_pop() == n);
        REQUIRE_FALSE(consumer.try_pop().has_value());
    }
}

// The counters can't wrap in practice: 2^64 items at a million per second
// take over half a million years. The arithmetic still has to be right.
TEST_CASE("SpscRing counters work across 64-bit wraparound", "[ipc][ring]") {
    SpscRing<std::uint64_t, 4> ring{};
    ring.head = std::numeric_limits<std::uint64_t>::max() - 1;
    ring.tail = ring.head;
    RingProducer producer{ring};
    RingConsumer consumer{ring};
    REQUIRE_FALSE(producer.corrupt());
    REQUIRE_FALSE(consumer.corrupt());

    for (std::uint64_t lap = 0; lap < 3; ++lap) {
        for (std::uint64_t n = 0; n < 4; ++n) {
            REQUIRE(producer.try_push((lap * 4) + n));
        }
        REQUIRE_FALSE(producer.try_push(0)); // full
        for (std::uint64_t n = 0; n < 4; ++n) {
            REQUIRE(consumer.try_pop() == (lap * 4) + n);
        }
        REQUIRE_FALSE(consumer.try_pop().has_value()); // empty
    }
    REQUIRE_FALSE(producer.corrupt());
    REQUIRE_FALSE(consumer.corrupt());
    REQUIRE(ring.head == 10); // wrapped past zero
}
