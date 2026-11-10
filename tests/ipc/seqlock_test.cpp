// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/seqlock.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {

using ks::ipc::Seqlock;
using ks::ipc::SeqlockReader;
using ks::ipc::SeqlockWriter;

// 12 bytes: the last of its two words is half padding.
struct Odd {
    std::uint32_t a;
    std::uint32_t b;
    std::uint32_t c;

    friend constexpr bool operator==(const Odd&, const Odd&) = default;
};

} // namespace

TEST_CASE("Seqlock carries values whose size isn't a whole number of words", "[ipc][seqlock]") {
    STATIC_REQUIRE(Seqlock<Odd>::kWords == 2);
    Seqlock<Odd> lock{};
    SeqlockWriter writer{lock};
    const SeqlockReader reader{lock};

    const Odd value{.a = 0x1111'1111, .b = 0x2222'2222, .c = 0x3333'3333};
    writer.write(value);
    REQUIRE(reader.try_read() == value);
}

TEST_CASE("SeqlockWriter continues from an even sequence number", "[ipc][seqlock]") {
    Seqlock<std::uint64_t> lock{};
    lock.sequence = 10;
    SeqlockWriter writer{lock};
    writer.write(42);
    REQUIRE(lock.sequence == 12);
    REQUIRE(SeqlockReader{lock}.try_read() == 42U);
}

TEST_CASE("SeqlockReader makes no attempt when allowed none", "[ipc][seqlock]") {
    Seqlock<std::uint64_t> lock{};
    REQUIRE_FALSE(SeqlockReader{lock}.try_read(0).has_value());
}
