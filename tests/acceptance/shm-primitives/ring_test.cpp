// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/shm-primitives/spec.md: the ring.

#include <ks/ipc/spsc_ring.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <thread>

namespace {

using ks::ipc::RingConsumer;
using ks::ipc::RingProducer;
using ks::ipc::SpscRing;
using ks::ipc::test::is_item;
using ks::ipc::test::Item;
using ks::ipc::test::make_item;
using ks::ipc::test::StallWatch;

} // namespace

TEST_CASE("Ring: every item arrives once, in order, across threads", "[ring]") {
    constexpr std::uint64_t kItems = 1'000'000;

    // A small ring, so the threads keep running into full and empty.
    SpscRing<Item, 8> ring{};
    std::string producer_error;
    std::string consumer_error;
    {
        const std::jthread producer_thread{[&ring, &producer_error] {
            RingProducer producer{ring};
            StallWatch watch;
            for (std::uint64_t n = 0; n < kItems;) {
                if (producer.try_push(make_item(n))) {
                    ++n;
                    watch.progressed();
                } else if (watch.stalled()) {
                    producer_error = std::format("no progress after pushing {} items", n);
                    return;
                } else {
                    std::this_thread::yield();
                }
            }
        }};
        const std::jthread consumer_thread{[&ring, &consumer_error] {
            RingConsumer consumer{ring};
            StallWatch watch;
            for (std::uint64_t n = 0; n < kItems;) {
                if (const std::optional<Item> item = consumer.try_pop()) {
                    if (!is_item(*item, n)) {
                        consumer_error = std::format(
                            "expected item {}, got item {} or a damaged copy", n, item->sequence);
                        return;
                    }
                    ++n;
                    watch.progressed();
                } else if (watch.stalled()) {
                    consumer_error = std::format("no progress after popping {} items", n);
                    return;
                } else {
                    std::this_thread::yield();
                }
            }
        }};
    }
    CAPTURE(producer_error, consumer_error);
    REQUIRE(producer_error.empty());
    REQUIRE(consumer_error.empty());
}

TEST_CASE("Ring: full and empty rings, and wrapping around", "[ring]") {
    SpscRing<Item, 4> ring{};
    RingProducer producer{ring};
    RingConsumer consumer{ring};

    // Zero-filled memory is an empty ring.
    REQUIRE_FALSE(consumer.try_pop().has_value());

    // Start one slot in, so that each lap below wraps around the end.
    REQUIRE(producer.try_push(make_item(0)));
    REQUIRE(consumer.try_pop() == make_item(0));

    std::uint64_t pushed = 1;
    std::uint64_t popped = 1;
    for (int lap = 0; lap < 3; ++lap) {
        for (int i = 0; i < 4; ++i) {
            REQUIRE(producer.try_push(make_item(pushed)));
            ++pushed;
        }
        REQUIRE_FALSE(producer.try_push(make_item(pushed))); // full

        for (int i = 0; i < 4; ++i) {
            REQUIRE(consumer.try_pop() == make_item(popped));
            ++popped;
        }
        REQUIRE_FALSE(consumer.try_pop().has_value()); // empty
    }

    REQUIRE_FALSE(producer.corrupt());
    REQUIRE_FALSE(consumer.corrupt());
    // The shared counters count items, not slots.
    REQUIRE(ring.head == pushed);
    REQUIRE(ring.tail == popped);
}

// The tests below write the ring's counters directly, as a buggy or malicious
// process could.

TEST_CASE("Ring: a consumer rejects a corrupt head", "[ring]") {
    SpscRing<Item, 4> ring{};

    SECTION("a head exactly Capacity ahead is a full ring, not corruption") {
        RingProducer producer{ring};
        for (std::uint64_t n = 0; n < 4; ++n) {
            REQUIRE(producer.try_push(make_item(n)));
        }
        RingConsumer consumer{ring};
        REQUIRE_FALSE(consumer.corrupt());
        for (std::uint64_t n = 0; n < 4; ++n) {
            REQUIRE(consumer.try_pop() == make_item(n));
        }
    }

    SECTION("too far ahead when attaching") {
        ring.head = 5;
        RingConsumer consumer{ring};
        REQUIRE(consumer.corrupt());
        REQUIRE_FALSE(consumer.try_pop().has_value());
        REQUIRE(ring.tail == 0);
    }

    SECTION("too far ahead while running") {
        RingProducer producer{ring};
        RingConsumer consumer{ring};
        REQUIRE(producer.try_push(make_item(0)));
        REQUIRE(producer.try_push(make_item(1)));
        REQUIRE(consumer.try_pop().has_value()); // the tail is now 1

        ring.head = 6; // 5 ahead of the tail
        REQUIRE_FALSE(consumer.try_pop().has_value());
        REQUIRE(consumer.corrupt());

        ring.head = 2; // put back
        REQUIRE_FALSE(consumer.try_pop().has_value());
        REQUIRE(consumer.corrupt());
        REQUIRE(ring.tail == 1); // never written again
    }

    SECTION("behind the tail") {
        RingProducer producer{ring};
        RingConsumer consumer{ring};
        for (std::uint64_t n = 0; n < 3; ++n) {
            REQUIRE(producer.try_push(make_item(n)));
            REQUIRE(consumer.try_pop().has_value());
        }

        ring.head = 2; // the tail is 3
        REQUIRE_FALSE(consumer.try_pop().has_value());
        REQUIRE(consumer.corrupt());
        REQUIRE(ring.tail == 3);
    }
}

TEST_CASE("Ring: a producer rejects a corrupt tail", "[ring]") {
    SpscRing<Item, 4> ring{};

    SECTION("when attaching") {
        ring.tail = 1; // more popped than pushed
        RingProducer producer{ring};
        REQUIRE(producer.corrupt());
        REQUIRE_FALSE(producer.try_push(make_item(0)));
        REQUIRE(ring.head == 0);
    }

    SECTION("when the ring is full") {
        RingProducer producer{ring};
        for (std::uint64_t n = 0; n < 4; ++n) {
            REQUIRE(producer.try_push(make_item(n)));
        }
        REQUIRE_FALSE(producer.try_push(make_item(4))); // full, not corrupt
        REQUIRE_FALSE(producer.corrupt());

        ring.tail = 5; // more popped than pushed
        REQUIRE_FALSE(producer.try_push(make_item(4)));
        REQUIRE(producer.corrupt());

        ring.tail = 4; // put back
        REQUIRE_FALSE(producer.try_push(make_item(4)));
        REQUIRE(producer.corrupt());
        REQUIRE(ring.head == 4); // never written again
    }
}

TEST_CASE("Ring: a new consumer or producer continues where the last one stopped", "[ring]") {
    SpscRing<Item, 4> ring{};
    {
        RingProducer producer{ring};
        for (std::uint64_t n = 0; n < 3; ++n) {
            REQUIRE(producer.try_push(make_item(n)));
        }
    }
    {
        RingConsumer consumer{ring};
        REQUIRE(consumer.try_pop() == make_item(0));
    }

    // As after an engine restart: the new consumer picks up at item 1.
    RingConsumer consumer{ring};
    REQUIRE_FALSE(consumer.corrupt());
    for (std::uint64_t n = 1; n < 3; ++n) {
        REQUIRE(consumer.try_pop() == make_item(n));
    }
    REQUIRE_FALSE(consumer.try_pop().has_value());

    // A new producer picks up at item 3.
    RingProducer producer{ring};
    REQUIRE_FALSE(producer.corrupt());
    REQUIRE(producer.try_push(make_item(3)));
    REQUIRE(consumer.try_pop() == make_item(3));
}
