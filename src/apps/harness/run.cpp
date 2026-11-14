// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/harness/run.h>

#include <ks/engine/audio_backend.h>
#include <ks/engine/block.h>
#include <ks/engine/interleave.h>
#include <ks/engine/miniaudio_backend.h>
#include <ks/engine/offline.h>
#include <ks/engine/output_controller.h>
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
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ks::harness {

bool flags_problem(const Report& report) noexcept {
    return (report.real_time && report.late_blocks > 0) || report.non_finite_samples > 0 ||
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

namespace {

using Clock = std::chrono::steady_clock;

/// The longest the control thread waits between polls (requirement 25).
constexpr std::chrono::milliseconds kPollInterval{5};
/// How long it waits, at the end, for the engine to read every command.
constexpr std::chrono::milliseconds kReadWait{100};
/// How often it prints a status line, in seconds.
constexpr double kStatusInterval = 60.0;

std::string_view state_name(engine::OutputState state) noexcept {
    switch (state) {
    case engine::OutputState::Stopped:
        return "Stopped";
    case engine::OutputState::Playing:
        return "Playing";
    case engine::OutputState::Freewheeling:
        return "Freewheeling";
    case engine::OutputState::Attaching:
        return "Attaching";
    }
    return "Unknown";
}

void print_stream(const engine::StreamInfo& info, std::ostream& out) {
    out << std::format("Backend: {}\n", info.backend);
    out << std::format("Device: {}\n", info.device);
    out << std::format("Format: {} Hz, {} channels, period {} frames, {} periods\n",
                       info.sample_rate, info.channels, info.period_frames, info.periods);
    out << std::format("Output latency: {:.3f} ms\n",
                       std::chrono::duration<double, std::milli>(info.output_latency).count());
}

/// Polls the controller and prints each change of output state with its
/// time, counting the times it starts freewheeling.
class StateWatcher {
public:
    StateWatcher(engine::OutputController& controller, Clock::time_point started,
                 std::ostream& out) noexcept
        : controller_{&controller}, started_{started}, out_{&out} {}

    void poll() {
        controller_->poll();
        note();
    }

    /// Prints the state if it has changed since the last call.
    void note() {
        const engine::OutputState state = controller_->state();
        if (state == shown_) {
            return;
        }
        shown_ = state;
        if (state == engine::OutputState::Freewheeling) {
            ++losses_;
        }
        *out_ << std::format("{:.3f} s: {}\n", seconds_since(started_), state_name(state));
    }

    [[nodiscard]] std::uint64_t losses() const noexcept { return losses_; }

    [[nodiscard]] static double seconds_since(Clock::time_point start) noexcept {
        return std::chrono::duration<double>(Clock::now() - start).count();
    }

private:
    engine::OutputController* controller_;
    Clock::time_point started_;
    std::ostream* out_;
    engine::OutputState shown_ = engine::OutputState::Stopped;
    std::uint64_t losses_ = 0;
};

} // namespace

std::expected<Report, std::string> play_on_device(const Script& script, const TrackSet& tracks,
                                                  const DeviceOptions& options,
                                                  const std::atomic<bool>& stop,
                                                  std::ostream& out) {
    std::expected<std::unique_ptr<Player>, std::string> created = Player::create(script, tracks);
    if (!created.has_value()) {
        return std::unexpected{std::move(created.error())};
    }
    Player& player = **created;
    const auto core = std::make_unique<engine::RenderCore>(player.engine());

    auto source = engine::MiniaudioDeviceSource::create(engine::MiniaudioSourceConfig{
        .backends = options.backends,
        .device = options.device,
        .channels = options.channels,
        .sample_rate = options.sample_rate,
        .period_frames = options.period_frames,
    });
    if (!source.has_value()) {
        return std::unexpected{std::move(source.error().message)};
    }

    // Open the device once, not started, to show what will play before
    // starting (requirement 25). The source then opens it in the same format.
    if (auto probe = (*source)->open(*core, engine::kDefaultDriver); probe.has_value()) {
        print_stream((*probe)->info(), out);
    } else {
        out << std::format("The output device couldn't be opened: {}\n", probe.error().message);
    }

    engine::OutputConfig config;
    if (options.sample_rate != 0) {
        config.default_sample_rate = options.sample_rate;
    }
    if (options.channels != 0) {
        config.default_channels = options.channels;
    }
    if (options.period_frames != 0) {
        config.freewheel_period_frames = options.period_frames;
    }
    engine::OutputController controller{*core, **source, config};

    Report report;
    report.real_time = true;
    const Clock::time_point started = Clock::now();
    StateWatcher watcher{controller, started, out};
    controller.start();
    watcher.note();

    double next_status = kStatusInterval;
    while (true) {
        watcher.poll();
        player.collect();
        if (stop.load(std::memory_order_relaxed)) {
            report.stopped_early = true;
            break;
        }
        const double now = StateWatcher::seconds_since(started);
        player.give_due(now);
        if (now >= script.end) {
            break;
        }
        if (now >= next_status) {
            Report so_far;
            player.fill(so_far);
            out << std::format("{:.3f} s: played {:.3f} s, {} late blocks, peak load {:.3f}\n", now,
                               so_far.played_seconds, so_far.late_blocks, so_far.peak_render_load);
            next_status += kStatusInterval;
        }
        // Until the next poll, the next event, or the end, whichever is first.
        double wake = std::min(script.end, next_status);
        if (const std::optional<double> next = player.next_time()) {
            wake = std::min(wake, *next);
        }
        const Clock::time_point poll_by = Clock::now() + kPollInterval;
        const Clock::time_point wake_at =
            started + std::chrono::duration_cast<Clock::duration>(
                          std::chrono::duration<double>(std::max(wake, now)));
        std::this_thread::sleep_until(std::min(poll_by, wake_at));
    }

    // Give the engine a moment to read every command sent, then stop.
    const Clock::time_point deadline = Clock::now() + kReadWait;
    while (!player.all_read() && Clock::now() < deadline) {
        watcher.poll();
        player.collect();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    controller.stop();
    watcher.note();

    player.collect();
    player.fill(report);
    report.device_losses = watcher.losses();
    return report;
}

} // namespace ks::harness
