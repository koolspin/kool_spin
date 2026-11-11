// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/track.h>

#include <algorithm>

namespace ks::engine {

namespace {

constexpr std::uint64_t kCompleteBit = std::uint64_t{1} << 63U;

} // namespace

Track::Track(std::span<const float> samples, std::uint32_t sample_rate) noexcept
    : samples_{samples}, sample_rate_{sample_rate} {}

void Track::publish(std::uint64_t frames, bool complete) noexcept KS_NONBLOCKING {
    ready_.store((frames & ~kCompleteBit) | (complete ? kCompleteBit : 0),
                 std::memory_order_release);
}

TrackReady Track::ready() const noexcept KS_NONBLOCKING {
    const std::uint64_t word = ready_.load(std::memory_order_acquire);
    return TrackReady{.frames = std::min(word & ~kCompleteBit, capacity()),
                      .complete = (word & kCompleteBit) != 0};
}

std::span<const float> Track::samples() const noexcept KS_NONBLOCKING {
    return samples_;
}

std::uint64_t Track::capacity() const noexcept KS_NONBLOCKING {
    return samples_.size() / 2;
}

std::uint32_t Track::sample_rate() const noexcept KS_NONBLOCKING {
    return sample_rate_;
}

} // namespace ks::engine
