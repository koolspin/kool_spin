// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/shm-primitives/spec.md: the seqlock.

#include <ks/ipc/seqlock.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using ks::ipc::Seqlock;
using ks::ipc::SeqlockReader;
using ks::ipc::SeqlockWriter;
using ks::ipc::test::is_consistent;
using ks::ipc::test::make_value;
using ks::ipc::test::StallWatch;
using ks::ipc::test::Value;

/// Reads `lock` while `writing` is true, counting successful reads in
/// `reads`. Returns an error message if a read is torn, or older than one
/// before it, or an empty string if every read was good.
std::string read_while_writing(Seqlock<Value>& lock, const std::atomic<bool>& writing,
                               std::atomic<std::uint64_t>& reads) {
    const SeqlockReader reader{lock};
    std::uint64_t newest = 0;
    while (writing.load(std::memory_order_acquire)) {
        const std::optional<Value> value = reader.try_read();
        if (!value) {
            std::this_thread::yield();
            continue;
        }
        // Before the first write, the value is all zeros.
        const std::uint64_t n = value->words[0];
        if (*value != Value{} && !is_consistent(*value)) {
            return std::format("torn value, starting as write {}", n);
        }
        if (n < newest) {
            return std::format("read write {} after write {}", n, newest);
        }
        newest = n;
        reads.fetch_add(1, std::memory_order_relaxed);
    }
    return {};
}

} // namespace

TEST_CASE("Seqlock: readers never see a torn value across threads", "[seqlock]") {
    constexpr std::uint64_t kMinWrites = 200'000;
    constexpr std::uint64_t kMinReadsEach = 1'000;
    constexpr std::size_t kReaders = 2;

    Seqlock<Value> lock{};
    std::atomic<bool> writing{true};
    // Successful reads by each reader while the writer was writing.
    std::array<std::atomic<std::uint64_t>, kReaders> reads{};
    std::array<std::string, kReaders> reader_errors;
    std::array<std::atomic<bool>, kReaders> reader_failed{};
    std::string writer_error;
    std::uint64_t last_write = 0;

    const auto fewest_reads = [&reads] {
        std::uint64_t fewest = std::numeric_limits<std::uint64_t>::max();
        for (const std::atomic<std::uint64_t>& count : reads) {
            fewest = std::min(fewest, count.load(std::memory_order_relaxed));
        }
        return fewest;
    };
    const auto any_reader_failed = [&reader_failed] {
        return std::ranges::any_of(reader_failed,
                                   [](const std::atomic<bool>& failed) { return failed.load(); });
    };

    {
        std::vector<std::jthread> readers;
        readers.reserve(kReaders);
        for (std::size_t r = 0; r < kReaders; ++r) {
            readers.emplace_back([&lock, &writing, &reads, &reader_errors, &reader_failed, r] {
                reader_errors.at(r) = read_while_writing(lock, writing, reads.at(r));
                reader_failed.at(r) = !reader_errors.at(r).empty();
            });
        }

        // Write until there have been enough writes and every reader has read
        // enough during them.
        SeqlockWriter writer{lock};
        StallWatch watch;
        std::uint64_t least_seen = 0;
        for (std::uint64_t n = 1; !any_reader_failed(); ++n) {
            writer.write(make_value(n));
            last_write = n;
            const std::uint64_t least = fewest_reads();
            if (least >= kMinReadsEach) {
                if (n >= kMinWrites) {
                    break;
                }
                watch.progressed();
            } else if (least != least_seen) {
                least_seen = least;
                watch.progressed();
            } else if (watch.stalled()) {
                writer_error = std::format(
                    "after {} writes, a reader has read only {} times successfully", n, least);
                break;
            }
        }
        writing = false;
    }

    CAPTURE(writer_error, reader_errors);
    REQUIRE(writer_error.empty());
    for (const std::string& error : reader_errors) {
        REQUIRE(error.empty());
    }
    REQUIRE(fewest_reads() >= kMinReadsEach);

    // With the writer finished, a read returns its last write.
    const SeqlockReader reader{lock};
    REQUIRE(reader.try_read() == make_value(last_write));
}

TEST_CASE("Seqlock: never written reads as zero", "[seqlock]") {
    Seqlock<Value> lock{};
    const SeqlockReader reader{lock};
    REQUIRE(reader.try_read() == Value{});
}

TEST_CASE("Seqlock: a write left half-done", "[seqlock]") {
    Seqlock<Value> lock{};
    {
        SeqlockWriter writer{lock};
        writer.write(make_value(7));
    }
    const std::uint64_t before = lock.sequence;
    REQUIRE(before == 2);

    // As if a writer died between its first and last store.
    ++lock.sequence;
    const SeqlockReader reader{lock};
    REQUIRE_FALSE(reader.try_read(5).has_value());

    // A new writer, as after an engine restart, makes it readable again.
    SeqlockWriter writer{lock};
    writer.write(make_value(8));
    REQUIRE(lock.sequence % 2 == 0);
    REQUIRE(lock.sequence > before);
    REQUIRE(reader.try_read() == make_value(8));
}
