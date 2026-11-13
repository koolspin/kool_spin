// SPDX-License-Identifier: GPL-3.0-or-later
#include "player.h"

#include <ks/engine/command.h>
#include <ks/engine/engine.h>
#include <ks/harness/encode.h>
#include <ks/harness/run.h>
#include <ks/harness/script.h>
#include <ks/harness/tracks.h>
#include <ks/ipc/command.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <variant>

namespace ks::harness {

std::uint64_t event_frame(double seconds, std::uint32_t sample_rate) noexcept {
    const double frame = seconds * static_cast<double>(sample_rate);
    if (!(frame >= 0.0)) { // NaN or negative: not a script time
        return 0;
    }
    return static_cast<std::uint64_t>(std::llround(frame));
}

namespace {

/// Whether every event in `script` names a deck and a track that exist.
bool events_valid(const Script& script, std::size_t tracks) {
    for (const Event& event : script.events) {
        const bool valid = std::visit(
            [&](const auto& action) {
                using T = std::decay_t<decltype(action)>;
                if constexpr (std::is_same_v<T, Load> || std::is_same_v<T, TrackTime>) {
                    return action.deck < engine::kDecks && action.track < tracks;
                } else if constexpr (std::is_same_v<T, Unload>) {
                    return action.deck < engine::kDecks;
                } else {
                    return true;
                }
            },
            event.action);
        if (!valid) {
            return false;
        }
    }
    return true;
}

} // namespace

std::expected<std::unique_ptr<Player>, std::string> Player::create(const Script& script,
                                                                   const TrackSet& tracks) {
    if (tracks.size() != script.tracks.size() || !events_valid(script, tracks.size())) {
        return std::unexpected{std::string{"the tracks weren't prepared from this script"}};
    }
    std::unique_ptr<Player> player{new Player{script, tracks}};
    const std::optional<std::size_t> client = player->engine_->attach(*player->ring_);
    if (!client.has_value()) {
        return std::unexpected{std::string{"the engine refused the command ring"}};
    }
    player->client_ = *client;
    return player;
}

Player::Player(const Script& script, const TrackSet& tracks)
    : script_{&script}, tracks_{&tracks}, state_{std::make_unique<ipc::EngineState>()},
      ring_{std::make_unique<ipc::CommandRing>()},
      engine_{std::make_unique<engine::Engine>(*state_)}, producer_{*ring_} {}

engine::Engine& Player::engine() noexcept {
    return *engine_;
}

std::optional<double> Player::next_time() const noexcept {
    if (next_ >= script_->events.size()) {
        return std::nullopt;
    }
    return std::span{script_->events}[next_].time;
}

void Player::give_due(double now) {
    give_while([now](double time) { return time <= now; });
}

void Player::give_due_at_frame(std::uint64_t frame, std::uint32_t sample_rate) {
    give_while([=](double time) { return event_frame(time, sample_rate) <= frame; });
}

template <typename Due>
void Player::give_while(const Due& due) {
    const std::span<const Event> events{script_->events};
    while (next_ < events.size() && due(events[next_].time)) {
        if (!give(events[next_])) {
            // This event and every later one that's due wait; count each
            // once.
            counted_ = std::max(counted_, next_);
            while (counted_ < events.size() && due(events[counted_].time)) {
                ++delayed_;
                ++counted_;
            }
            return;
        }
        ++next_;
    }
}

bool Player::give(const Event& event) {
    return std::visit(
        [&](const auto& action) -> bool {
            using T = std::decay_t<decltype(action)>;
            if constexpr (std::is_same_v<T, Load>) {
                return engine_->load(action.deck, tracks_->track(action.track));
            } else if constexpr (std::is_same_v<T, Unload>) {
                return engine_->unload(action.deck);
            } else {
                engine::Command command{};
                if constexpr (std::is_same_v<T, TrackTime>) {
                    command = command_for(action, tracks_->track(action.track).sample_rate());
                } else {
                    command = action;
                }
                if (!producer_.try_push(encode(command, sequence_ + 1))) {
                    return false;
                }
                ++sequence_;
                return true;
            }
        },
        event.action);
}

void Player::collect() noexcept {
    while (engine_->collect().has_value()) {
    }
}

void Player::read_status() {
    const ipc::SeqlockReader<ipc::EngineStatus> reader{state_->status};
    if (const std::optional<ipc::EngineStatus> status = reader.try_read()) {
        status_ = *status;
    }
}

bool Player::all_read() {
    read_status();
    return std::span{status_.clients}[client_].commands_read >= sequence_;
}

void Player::fill(Report& report) {
    read_status();
    const ipc::ClientAck ack = std::span{status_.clients}[client_];
    report.played_seconds = status_.sample_rate > 0 ? static_cast<double>(status_.frames_rendered) /
                                                          static_cast<double>(status_.sample_rate)
                                                    : 0.0;
    report.blocks_rendered = status_.heartbeat;
    report.late_blocks = status_.late_blocks;
    report.peak_render_load = std::bit_cast<double>(status_.peak_render_load);
    report.non_finite_samples = status_.non_finite_samples;
    report.limited_frames = status_.limited_frames;
    report.commands_sent = sequence_;
    report.commands_read = ack.commands_read;
    report.commands_rejected = ack.rejected;
    report.events_delayed = delayed_;
}

std::uint64_t Player::commands_sent() const noexcept {
    return sequence_;
}

std::uint64_t Player::events_delayed() const noexcept {
    return delayed_;
}

} // namespace ks::harness
