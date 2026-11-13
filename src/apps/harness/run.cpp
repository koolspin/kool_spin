// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/harness/run.h>

#include <ks/engine/block.h>
#include <ks/engine/interleave.h>
#include <ks/engine/offline.h>
#include <ks/engine/render_core.h>
#include <ks/harness/script.h>
#include <ks/harness/tracks.h>

#include "player.h"
#include <ks_codecs.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ks::harness {

bool flags_problem(const Report& report) noexcept {
    return report.late_blocks > 0 || report.non_finite_samples > 0 ||
           report.commands_rejected > 0 || report.commands_read < report.commands_sent;
}

void print_report(const Report& report, std::ostream& out) {
    out << std::format("Played {:.3f} s\n", report.played_seconds);
    if (report.stopped_early) {
        out << "Stopped early\n";
    }
    out << std::format("Blocks: {} rendered, {} late, peak load {:.3f}\n", report.blocks_rendered,
                       report.late_blocks, report.peak_render_load);
    out << std::format("Samples: {} non-finite, {} limited\n", report.non_finite_samples,
                       report.limited_frames);
    out << std::format("Commands: {} sent, {} read, {} rejected, {} delayed\n",
                       report.commands_sent, report.commands_read, report.commands_rejected,
                       report.events_delayed);
    out << std::format("Device losses: {}\n", report.device_losses);
}

namespace {

/// An open WAV file, closed (once) by close() or the destructor.
class WavFile {
public:
    WavFile() = default;
    WavFile(const WavFile&) = delete;
    WavFile& operator=(const WavFile&) = delete;
    WavFile(WavFile&&) = delete;
    WavFile& operator=(WavFile&&) = delete;
    ~WavFile() { static_cast<void>(close()); }

    [[nodiscard]] int open(const std::filesystem::path& path, std::uint32_t channels,
                           std::uint32_t sample_rate) {
        return ks_codecs_wav_open(path.string().c_str(), channels, sample_rate, &writer_);
    }

    [[nodiscard]] int write(std::span<const float> samples, std::uint64_t frames) {
        return ks_codecs_wav_write(writer_, samples.data(), frames);
    }

    [[nodiscard]] int close() {
        if (writer_ == nullptr) {
            return KS_CODECS_OK;
        }
        ks_codecs_wav_writer* const writer = std::exchange(writer_, nullptr);
        return ks_codecs_wav_close(writer);
    }

private:
    ks_codecs_wav_writer* writer_ = nullptr;
};

std::string file_error(const std::filesystem::path& path, int result) {
    return std::format("{}: {}", path.string(), ks_codecs_describe(result));
}

} // namespace

std::expected<Report, std::string> render_to_file(const Script& script, const TrackSet& tracks,
                                                  const FileOptions& options,
                                                  const std::atomic<bool>& stop) {
    const std::uint32_t rate = options.sample_rate;
    const std::size_t channels = options.channels;
    const std::size_t block = options.block_frames;
    if (rate == 0 || channels == 0 || channels > engine::kMaxOutputChannels || block == 0) {
        return std::unexpected{std::string{"the output's rate, channels, or block size is 0 "
                                           "or out of range"}};
    }

    std::expected<std::unique_ptr<Player>, std::string> created = Player::create(script, tracks);
    if (!created.has_value()) {
        return std::unexpected{std::move(created.error())};
    }
    Player& player = **created;
    const auto core = std::make_unique<engine::RenderCore>(player.engine());

    WavFile file;
    if (const int result = file.open(options.path, options.channels, rate);
        result != KS_CODECS_OK) {
        return std::unexpected{file_error(options.path, result)};
    }

    std::vector<std::vector<float>> planar(channels, std::vector<float>(block));
    std::vector<std::span<float>> spans;
    spans.reserve(channels);
    std::vector<float> interleaved(block * channels);

    // A fixed start time, so host times are the same on every run.
    const std::chrono::steady_clock::time_point start{};
    const std::uint64_t end_frame = event_frame(script.end, rate);
    Report report;
    std::uint64_t frame = 0;
    player.give_due_at_frame(frame, rate);
    while (frame < end_frame) {
        if (stop.load(std::memory_order_relaxed)) {
            report.stopped_early = true;
            break;
        }
        // Up to the next event's frame, if it's later than this one (an event
        // due now is waiting for room and is retried after the block), the
        // end, and one block.
        std::uint64_t until = std::min<std::uint64_t>(end_frame, frame + block);
        if (const std::optional<double> next = player.next_time()) {
            const std::uint64_t next_frame = event_frame(*next, rate);
            if (next_frame > frame) {
                until = std::min<std::uint64_t>(until, next_frame);
            }
        }
        const auto frames = static_cast<std::size_t>(until - frame);

        spans.clear();
        for (std::vector<float>& samples : planar) {
            spans.emplace_back(std::span{samples}.first(frames));
        }
        const engine::OfflineConfig config{
            .sample_rate = rate,
            .block_frames = frames,
            .start_time = start + engine::frames_to_duration(frame, rate),
        };
        if (engine::render_offline(*core, config, spans) != frames) {
            return std::unexpected{std::string{"the engine didn't render a block"}};
        }
        if (!engine::interleave(spans, frames, interleaved)) {
            return std::unexpected{std::string{"couldn't interleave a block"}};
        }
        if (const int result = file.write(interleaved, frames); result != KS_CODECS_OK) {
            return std::unexpected{file_error(options.path, result)};
        }
        frame = until;
        player.collect();
        player.give_due_at_frame(frame, rate);
    }

    if (const int result = file.close(); result != KS_CODECS_OK) {
        return std::unexpected{file_error(options.path, result)};
    }
    player.collect();
    player.fill(report);
    return report;
}

// Stub (specs/harness/spec.md requirements 17 to 20 and 25 to 27).
std::expected<Report, std::string> play_on_device(const Script& /*script*/,
                                                  const TrackSet& /*tracks*/,
                                                  const DeviceOptions& /*options*/,
                                                  const std::atomic<bool>& /*stop*/,
                                                  std::ostream& /*out*/) {
    return std::unexpected{std::string{"not implemented"}};
}

} // namespace ks::harness
