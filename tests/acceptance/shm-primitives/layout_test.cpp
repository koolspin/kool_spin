// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/shm-primitives/spec.md: the shared layouts, and
// real-time safety.

#include <ks/ipc/layout.h>

#include <ks/core/realtime.h>
#include <ks/ipc/region.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/spsc_ring.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace {

using ks::ipc::kSegmentHeaderSize;
using ks::ipc::kSharedAlignment;
using ks::ipc::RingConsumer;
using ks::ipc::RingProducer;
using ks::ipc::SegmentHeader;
using ks::ipc::Seqlock;
using ks::ipc::SeqlockReader;
using ks::ipc::SeqlockWriter;
using ks::ipc::SpscRing;
using ks::ipc::test::Item;
using ks::ipc::test::make_item;
using ks::ipc::test::make_value;
using ks::ipc::test::Value;

using Ring = SpscRing<Item, 8>;
using Lock = Seqlock<Value>;

struct Exchanged {
    bool pushed = false;
    std::optional<Item> popped;
    std::optional<Value> read;
    bool producer_corrupt = false;
    bool consumer_corrupt = false;
};

// Calls every ring and seqlock operation from KS_NONBLOCKING code. Function
// effect analysis rejects the build if any of them isn't KS_NONBLOCKING, and
// under the rtsan preset RealtimeSanitizer checks what they do at run time.
Exchanged exchange_once(RingProducer<Item, 8>& producer, RingConsumer<Item, 8>& consumer,
                        SeqlockWriter<Value>& writer, const SeqlockReader<Value>& reader,
                        const Item& item, const Value& value) noexcept KS_NONBLOCKING {
    const bool pushed = producer.try_push(item);
    const std::optional<Item> popped = consumer.try_pop();
    writer.write(value);
    const std::optional<Value> read = reader.try_read();
    return {.pushed = pushed,
            .popped = popped,
            .read = read,
            .producer_corrupt = producer.corrupt(),
            .consumer_corrupt = consumer.corrupt()};
}

} // namespace

TEST_CASE("Layouts: shared structs follow ADR 0003", "[layout]") {
    STATIC_REQUIRE(std::is_trivially_copyable_v<Ring>);
    STATIC_REQUIRE(std::is_standard_layout_v<Ring>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Lock>);
    STATIC_REQUIRE(std::is_standard_layout_v<Lock>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<SegmentHeader>);
    STATIC_REQUIRE(std::is_standard_layout_v<SegmentHeader>);

    STATIC_REQUIRE(std::atomic_ref<std::uint64_t>::is_always_lock_free);

    // Fields that different processes write are a shared alignment apart.
    STATIC_REQUIRE(kSharedAlignment == 128);
    STATIC_REQUIRE(offsetof(Ring, tail) - offsetof(Ring, head) >= kSharedAlignment);
    STATIC_REQUIRE(offsetof(Ring, slots) - offsetof(Ring, tail) >= kSharedAlignment);

    // The seqlock holds the whole value.
    STATIC_REQUIRE(Lock::kWords * sizeof(std::uint64_t) >= sizeof(Value));

    // The header fits in its space, and a layout can start right after it.
    STATIC_REQUIRE(sizeof(SegmentHeader) <= kSegmentHeaderSize);
    STATIC_REQUIRE(kSegmentHeaderSize % alignof(Ring) == 0);
    STATIC_REQUIRE(kSegmentHeaderSize % alignof(Lock) == 0);
}

TEST_CASE("Ring and seqlock operations are real-time safe", "[layout]") {
    Ring ring{};
    Lock lock{};
    RingProducer producer{ring};
    RingConsumer consumer{ring};
    SeqlockWriter writer{lock};
    const SeqlockReader reader{lock};

    const Item item = make_item(1);
    const Value value = make_value(1);
    const Exchanged result = exchange_once(producer, consumer, writer, reader, item, value);
    REQUIRE(result.pushed);
    REQUIRE(result.popped == item);
    REQUIRE(result.read == value);
    REQUIRE_FALSE(result.producer_corrupt);
    REQUIRE_FALSE(result.consumer_corrupt);
}
