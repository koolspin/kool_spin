// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the shm-primitives acceptance tests: the values they pass through
// rings and seqlocks, a watchdog that turns a hang into a failure, and a
// thread that joins when destroyed.

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <utility>

namespace ks::ipc::test {

/// A word derived from `n` and `i` that differs from its neighbors in many bit
/// positions, so a torn or stale copy can't pass a check by accident.
constexpr std::uint64_t mix(std::uint64_t n, std::size_t i) noexcept {
    std::uint64_t x = (n * 0x9E37'79B9'7F4A'7C15ULL) + (i * 0xBF58'476D'1CE4'E5B9ULL) + 1;
    x ^= x >> 31U;
    x *= 0x94D0'49BB'1331'11EBULL;
    x ^= x >> 29U;
    return x;
}

/// A 64-byte ring item: a sequence number, and words derived from it.
struct Item {
    std::uint64_t sequence;
    std::array<std::uint64_t, 7> check;

    friend constexpr bool operator==(const Item&, const Item&) = default;
};
static_assert(sizeof(Item) == 64);

/// Item number `n`.
constexpr Item make_item(std::uint64_t n) noexcept {
    Item item{.sequence = n, .check = {}};
    for (std::size_t i = 0; i < item.check.size(); ++i) {
        item.check.at(i) = mix(n, i);
    }
    return item;
}

/// True if `item` is item number `n`, intact.
constexpr bool is_item(const Item& item, std::uint64_t n) noexcept {
    return item == make_item(n);
}

/// A 256-byte seqlock value: words[0] is a write number, and every other word
/// is derived from it.
struct Value {
    std::array<std::uint64_t, 32> words;

    friend constexpr bool operator==(const Value&, const Value&) = default;
};
static_assert(sizeof(Value) == 256);

/// The value of write number `n`.
constexpr Value make_value(std::uint64_t n) noexcept {
    Value value{};
    value.words.at(0) = n;
    for (std::size_t i = 1; i < value.words.size(); ++i) {
        value.words.at(i) = mix(n, i);
    }
    return value;
}

/// True if every word of `value` comes from the same write.
constexpr bool is_consistent(const Value& value) noexcept {
    return value == make_value(value.words.at(0));
}

/// How long a stress test waits without progress before failing.
inline constexpr std::chrono::seconds kStallLimit{5};

/// Turns a hang into a failure: call progressed() whenever something moves,
/// and give up once stalled() is true.
class StallWatch {
public:
    void progressed() noexcept { last_progress_ = Clock::now(); }

    [[nodiscard]] bool stalled() const noexcept {
        return Clock::now() - last_progress_ > kStallLimit;
    }

private:
    using Clock = std::chrono::steady_clock;

    Clock::time_point last_progress_ = Clock::now();
};

/// A thread that joins when destroyed: the part of std::jthread these tests
/// use. AppleClang 17's libc++ doesn't provide std::jthread.
class JoiningThread {
public:
    template <typename F>
        requires(!std::same_as<std::remove_cvref_t<F>, JoiningThread>)
    explicit JoiningThread(F&& function) : thread_{std::forward<F>(function)} {}

    JoiningThread(const JoiningThread&) = delete;
    JoiningThread& operator=(const JoiningThread&) = delete;
    JoiningThread(JoiningThread&&) noexcept = default;
    JoiningThread& operator=(JoiningThread&&) = delete;

    ~JoiningThread() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    std::thread thread_;
};

} // namespace ks::ipc::test
