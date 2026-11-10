// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/offline.h>

#include <algorithm>
#include <array>

namespace ks::engine {

std::size_t render_offline(RenderCore& core, const OfflineConfig& config,
                           std::span<const std::span<float>> output) noexcept KS_NONBLOCKING {
    if (output.empty() || output.size() > kMaxOutputChannels || config.block_frames == 0 ||
        config.sample_rate == 0) {
        return 0;
    }
    const std::size_t frames = output.front().size();
    for (const std::span<float> channel : output) {
        if (channel.size() != frames) {
            return 0;
        }
    }

    // Each block's channels are views into `output`.
    std::array<std::span<float>, kMaxOutputChannels> views{};
    const std::span<std::span<float>> channels = std::span{views}.first(output.size());

    std::size_t rendered = 0;
    std::size_t offset = 0;
    while (offset < frames) {
        // One configured block, rendered in pieces of at most kMaxBlockFrames.
        const std::size_t block_end = offset + std::min(config.block_frames, frames - offset);
        while (offset < block_end) {
            const std::size_t count = std::min(block_end - offset, kMaxBlockFrames);
            for (std::size_t c = 0; c < channels.size(); ++c) {
                channels[c] = output[c].subspan(offset, count);
            }
            const Block block{
                .channels = channels,
                .frames = count,
                .sample_rate = config.sample_rate,
                .host_time = config.start_time + frames_to_duration(offset, config.sample_rate),
            };
            if (core.try_render(block, config.driver) == RenderResult::Rendered) {
                rendered += count;
            }
            offset += count;
        }
    }
    return rendered;
}

} // namespace ks::engine
