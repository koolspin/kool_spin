// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/shm-primitives/spec.md: regions, segment headers,
// and the ring and seqlock across real processes.

#include <ks/ipc/region.h>

#include <ks/ipc/seqlock.h>
#include <ks/ipc/spsc_ring.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>

namespace {

using ks::ipc::check_header;
using ks::ipc::HeaderError;
using ks::ipc::kSegmentHeaderSize;
using ks::ipc::kSharedAlignment;
using ks::ipc::Region;
using ks::ipc::RingConsumer;
using ks::ipc::RingProducer;
using ks::ipc::SegmentHeader;
using ks::ipc::SegmentKind;
using ks::ipc::Seqlock;
using ks::ipc::SeqlockReader;
using ks::ipc::SeqlockWriter;
using ks::ipc::SpscRing;
using ks::ipc::write_header;
using ks::ipc::test::is_consistent;
using ks::ipc::test::is_item;
using ks::ipc::test::Item;
using ks::ipc::test::make_item;
using ks::ipc::test::make_value;
using ks::ipc::test::StallWatch;
using ks::ipc::test::Value;

/// A forked child. If a test fails while the child is still running, the
/// child is killed rather than left behind.
class Child {
public:
    explicit Child(pid_t pid) noexcept : pid_{pid} {}
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&&) = delete;
    Child& operator=(Child&&) = delete;

    ~Child() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            static_cast<void>(wait());
        }
    }

    /// Waits for the child to finish. Returns its exit code, or -1 if it
    /// didn't exit normally.
    int wait() noexcept {
        int status = 0;
        pid_t result = ::waitpid(pid_, &status, 0);
        while (result == -1 && errno == EINTR) {
            result = ::waitpid(pid_, &status, 0);
        }
        pid_ = 0;
        if (result == -1 || !WIFEXITED(status)) {
            return -1;
        }
        return WEXITSTATUS(status);
    }

private:
    pid_t pid_;
};

/// The error check_header() reports, or nothing if it accepts the segment.
std::optional<HeaderError> refusal(std::span<const std::byte> segment, SegmentKind kind,
                                   std::uint32_t layout_version) {
    const std::expected<void, HeaderError> result = check_header(segment, kind, layout_version);
    if (result) {
        return std::nullopt;
    }
    return result.error();
}

} // namespace

TEST_CASE("Region: anonymous memory is zero-filled and shared with a forked child", "[region]") {
    REQUIRE_FALSE(Region::create_anonymous(0).has_value());

    const std::expected<Region, std::error_code> region = Region::create_anonymous(10'000);
    REQUIRE(region.has_value());
    const std::span<std::byte> bytes = region->bytes();
    const auto page_size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    REQUIRE(bytes.size() >= 10'000);
    REQUIRE(bytes.size() % page_size == 0);
    REQUIRE(std::ranges::all_of(bytes, [](std::byte b) { return b == std::byte{0}; }));

    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        std::ranges::fill(bytes, std::byte{0xA5});
        ::_exit(0);
    }
    Child child{pid};
    REQUIRE(child.wait() == 0);
    REQUIRE(std::ranges::all_of(bytes, [](std::byte b) { return b == std::byte{0xA5}; }));
}

TEST_CASE("Region: a ring and a seqlock work across processes", "[region]") {
    struct Layout {
        SpscRing<Item, 8> ring;
        Seqlock<Value> newest;
    };
    constexpr std::uint64_t kItems = 200'000;
    constexpr std::uint32_t kLayoutVersion = 1;

    const std::expected<Region, std::error_code> region =
        Region::create_anonymous(kSegmentHeaderSize + sizeof(Layout));
    REQUIRE(region.has_value());
    write_header(region->bytes(), SegmentKind::CommandRing, kLayoutVersion);
    REQUIRE(check_header(region->bytes(), SegmentKind::CommandRing, kLayoutVersion).has_value());
    auto* const layout = region->at<Layout>(kSegmentHeaderSize);
    REQUIRE(layout != nullptr);

    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // The child pushes every item, and publishes the newest in the seqlock.
        RingProducer producer{layout->ring};
        SeqlockWriter writer{layout->newest};
        StallWatch watch;
        for (std::uint64_t n = 0; n < kItems;) {
            if (producer.try_push(make_item(n))) {
                writer.write(make_value(n));
                ++n;
                watch.progressed();
            } else if (watch.stalled()) {
                ::_exit(1);
            } else {
                std::this_thread::yield();
            }
        }
        ::_exit(0);
    }
    Child child{pid};

    // The parent pops every item, and reads the seqlock as it goes.
    RingConsumer consumer{layout->ring};
    const SeqlockReader reader{layout->newest};
    StallWatch watch;
    std::uint64_t newest = 0;
    std::string error;
    for (std::uint64_t n = 0; n < kItems && error.empty();) {
        if (const std::optional<Item> item = consumer.try_pop()) {
            if (!is_item(*item, n)) {
                error = std::format("expected item {}, got item {} or a damaged copy", n,
                                    item->sequence);
            }
            ++n;
            watch.progressed();
        } else if (watch.stalled()) {
            error = std::format("no progress after popping {} items", n);
        } else {
            std::this_thread::yield();
        }

        // Before the child's first write, the value is all zeros.
        const std::optional<Value> value = reader.try_read();
        if (value && *value != Value{}) {
            if (!is_consistent(*value)) {
                error = std::format("torn value, starting as write {}", value->words[0]);
            } else if (value->words[0] < newest) {
                error = std::format("read write {} after write {}", value->words[0], newest);
            } else {
                newest = value->words[0];
            }
        }
    }
    CAPTURE(error);
    REQUIRE(error.empty());
    REQUIRE(child.wait() == 0);

    REQUIRE(reader.try_read() == make_value(kItems - 1));
}

TEST_CASE("Region: at() refuses what doesn't fit", "[region]") {
    using Ring = SpscRing<Item, 8>;

    const std::expected<Region, std::error_code> region = Region::create_anonymous(4096);
    REQUIRE(region.has_value());
    const std::span<std::byte> bytes = region->bytes();
    const std::size_t size = bytes.size();

    // Fits, at the right address.
    REQUIRE(static_cast<void*>(region->at<std::uint64_t>(0)) == static_cast<void*>(bytes.data()));
    REQUIRE(static_cast<void*>(region->at<std::uint64_t>(size - 8)) ==
            static_cast<void*>(bytes.subspan(size - 8).data()));
    REQUIRE(static_cast<void*>(region->at<Ring>(kSharedAlignment)) ==
            static_cast<void*>(bytes.subspan(kSharedAlignment).data()));

    // Doesn't fit.
    REQUIRE(region->at<std::uint64_t>(size) == nullptr);
    REQUIRE(region->at<Ring>(size - kSharedAlignment) == nullptr);

    // Overflows: offset + sizeof(T) wraps around to a small number.
    REQUIRE(region->at<std::uint64_t>(std::numeric_limits<std::size_t>::max() - 7) == nullptr);

    // Misaligned.
    REQUIRE(region->at<std::uint64_t>(4) == nullptr);
    REQUIRE(region->at<Ring>(64) == nullptr);
}

TEST_CASE("Segment header: mismatches are refused", "[region]") {
    constexpr std::size_t kSize = 256;
    alignas(kSharedAlignment) std::array<std::byte, kSize> segment{};
    write_header(segment, SegmentKind::Track, 3);
    REQUIRE_FALSE(refusal(segment, SegmentKind::Track, 3).has_value());

    // Rewrites one field of the header, as another process could.
    const auto tamper = [&segment](auto change) {
        SegmentHeader header{};
        std::memcpy(&header, segment.data(), sizeof header);
        change(header);
        std::memcpy(segment.data(), &header, sizeof header);
    };

    SECTION("smaller than a header") {
        REQUIRE(refusal(std::span{segment}.first(kSegmentHeaderSize - 1), SegmentKind::Track, 3) ==
                HeaderError::TooSmall);
    }
    SECTION("not a kool_spin segment") {
        tamper([](SegmentHeader& header) { header.magic ^= 1U; });
        REQUIRE(refusal(segment, SegmentKind::Track, 3) == HeaderError::BadMagic);
    }
    SECTION("a different layout version") {
        REQUIRE(refusal(segment, SegmentKind::Track, 4) == HeaderError::BadVersion);
    }
    SECTION("a different kind") {
        REQUIRE(refusal(segment, SegmentKind::CommandRing, 3) == HeaderError::BadKind);
    }
    SECTION("not a kind at all") {
        tamper([](SegmentHeader& header) { header.kind = 99; });
        REQUIRE(refusal(segment, SegmentKind::Track, 3) == HeaderError::BadKind);
    }
    SECTION("a size that doesn't match the mapping") {
        REQUIRE(refusal(std::span{segment}.first(kSegmentHeaderSize), SegmentKind::Track, 3) ==
                HeaderError::BadSize);
    }
}
