// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/region.h>

#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

namespace {

using ks::ipc::kSegmentHeaderSize;
using ks::ipc::kSegmentMagic;
using ks::ipc::Region;
using ks::ipc::SegmentHeader;
using ks::ipc::SegmentKind;

std::size_t page_size() {
    return static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
}

} // namespace

TEST_CASE("Region keeps a size that is already a whole number of pages", "[ipc][region]") {
    auto region = Region::create_anonymous(page_size());
    REQUIRE(region.has_value());
    REQUIRE(region->bytes().size() == page_size());
}

TEST_CASE("Region refuses a size that can't be rounded up to a page", "[ipc][region]") {
    const auto region = Region::create_anonymous(std::numeric_limits<std::size_t>::max());
    REQUIRE_FALSE(region.has_value());
}

TEST_CASE("Region moves its mapping", "[ipc][region]") {
    auto created = Region::create_anonymous(1);
    REQUIRE(created.has_value());
    const std::span<std::byte> original = created->bytes();
    original.front() = std::byte{7};

    Region moved{std::move(*created)};
    // NOLINTNEXTLINE(bugprone-use-after-move): checking the moved-from state
    REQUIRE(created->bytes().empty());
    REQUIRE(moved.bytes().data() == original.data());
    REQUIRE(moved.bytes().front() == std::byte{7});

    auto other = Region::create_anonymous(1);
    REQUIRE(other.has_value());
    *other = std::move(moved);
    // NOLINTNEXTLINE(bugprone-use-after-move): checking the moved-from state
    REQUIRE(moved.bytes().empty());
    REQUIRE(other->bytes().data() == original.data());
}

TEST_CASE("write_header records the magic number, kind, version, and size", "[ipc][region]") {
    auto region = Region::create_anonymous(1);
    REQUIRE(region.has_value());
    const std::span<std::byte> bytes = region->bytes();
    ks::ipc::write_header(bytes, SegmentKind::EngineState, 7);

    SegmentHeader header{};
    std::memcpy(&header, bytes.data(), sizeof header);
    REQUIRE(header.magic == kSegmentMagic);
    REQUIRE(header.layout_version == 7);
    REQUIRE(header.kind == std::to_underlying(SegmentKind::EngineState));
    REQUIRE(header.size == bytes.size());
    REQUIRE(bytes.size() >= kSegmentHeaderSize);
}
