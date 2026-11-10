// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/block.h>

#include <limits>

namespace ks::engine {

std::chrono::nanoseconds frames_to_duration(std::uint64_t frames,
                                            std::uint32_t sample_rate) noexcept KS_NONBLOCKING {
    if (sample_rate == 0) {
        return {};
    }
    constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000;
    constexpr auto kMaxNanoseconds =
        static_cast<std::uint64_t>(std::numeric_limits<std::chrono::nanoseconds::rep>::max());

    // Whole seconds, then the rest. The rest is below one second's frames, so
    // `remainder * 10⁹` is below 2³² × 10⁹ and can't overflow.
    const std::uint64_t seconds = frames / sample_rate;
    const std::uint64_t remainder = frames % sample_rate;
    if (seconds >= kMaxNanoseconds / kNanosecondsPerSecond) {
        return std::chrono::nanoseconds::max(); // about 292 years
    }
    const std::uint64_t nanoseconds =
        (seconds * kNanosecondsPerSecond) + (remainder * kNanosecondsPerSecond / sample_rate);
    return std::chrono::nanoseconds{static_cast<std::chrono::nanoseconds::rep>(nanoseconds)};
}

} // namespace ks::engine
