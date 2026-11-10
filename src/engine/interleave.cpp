// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/interleave.h>

#include <algorithm>
#include <array>

namespace ks::engine {

bool interleave(std::span<const std::span<float>> planar, std::size_t frames,
                std::span<float> interleaved) noexcept KS_NONBLOCKING {
    const std::size_t channels = planar.size();
    // frames * channels > interleaved.size(), without overflowing.
    if (channels != 0 && frames > interleaved.size() / channels) {
        return false;
    }
    for (const std::span<float> channel : planar) {
        if (channel.size() < frames) {
            return false;
        }
    }
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t c = 0; c < channels; ++c) {
            interleaved[(f * channels) + c] = planar[c][f];
        }
    }
    return true;
}

namespace {

bool consistent(std::span<const std::span<float>> scratch, std::span<const float> interleaved,
                std::size_t channels, std::uint32_t sample_rate) noexcept KS_NONBLOCKING {
    if (channels == 0 || channels > kMaxOutputChannels || scratch.size() < channels ||
        sample_rate == 0 || interleaved.size() % channels != 0) {
        return false;
    }
    return std::ranges::all_of(scratch.first(channels), [](std::span<float> buffer) {
        return buffer.size() >= kMaxBlockFrames;
    });
}

} // namespace

std::size_t render_interleaved(RenderCore& core, std::span<const std::span<float>> scratch,
                               std::span<float> interleaved, std::size_t channels,
                               std::uint32_t sample_rate,
                               std::chrono::steady_clock::time_point host_time,
                               DriverId /*driver*/) noexcept KS_NONBLOCKING {
    if (!consistent(scratch, interleaved, channels, sample_rate)) {
        std::ranges::fill(interleaved, 0.0F);
        return 0;
    }
    const std::size_t frames = interleaved.size() / channels;

    // Each block's channels are views into the scratch buffers.
    std::array<std::span<float>, kMaxOutputChannels> views{};
    const std::span<std::span<float>> block_channels = std::span{views}.first(channels);

    std::size_t rendered = 0;
    for (std::size_t offset = 0; offset < frames;) {
        const std::size_t count = std::min(frames - offset, kMaxBlockFrames);
        for (std::size_t c = 0; c < channels; ++c) {
            block_channels[c] = scratch[c].first(count);
        }
        const Block block{
            .channels = block_channels,
            .frames = count,
            .sample_rate = sample_rate,
            .host_time = host_time + frames_to_duration(offset, sample_rate),
        };
        const std::span<float> destination =
            interleaved.subspan(offset * channels, count * channels);
        switch (core.try_render(block)) {
        case RenderResult::Rendered:
            rendered += count;
            [[fallthrough]];
        case RenderResult::Skipped: // the core silenced the block
            static_cast<void>(interleave(block_channels, count, destination));
            break;
        case RenderResult::Rejected: // can't happen: the arguments were checked
            std::ranges::fill(destination, 0.0F);
            break;
        }
        offset += count;
    }
    return rendered;
}

} // namespace ks::engine
