// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What every script read_script() accepts must satisfy (specs/harness/spec.md
// requirements 8 to 15). The fuzz target checks it on every input it accepts;
// "Script: tracks and timed commands" checks it in every preset, so this
// header is compiled and linted everywhere.

#include <ks/engine/block.h>
#include <ks/engine/command.h>
#include <ks/engine/mixer.h>
#include <ks/engine/track.h>
#include <ks/harness/encode.h>
#include <ks/harness/script.h>
#include <ks/ipc/command.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace ks::harness::test {

namespace properties_detail {

inline bool valid_signal(const Signal& signal) {
    const double rate = signal.sample_rate;
    const double nyquist = rate / 2.0;
    const bool rate_ok =
        signal.sample_rate >= engine::kMinTrackRate && signal.sample_rate <= engine::kMaxTrackRate;
    const bool length_ok = signal.seconds > 0.0 && signal.seconds <= kMaxSignalSeconds;
    switch (signal.kind) {
    case SignalKind::Sine:
        return rate_ok && length_ok && signal.frequency_hz > 0.0 && signal.frequency_hz < nyquist;
    case SignalKind::Sweep:
        return rate_ok && length_ok && signal.frequency_hz > 0.0 && signal.frequency_hz < nyquist &&
               signal.to_hz > 0.0 && signal.to_hz < nyquist;
    case SignalKind::Noise:
        return rate_ok && length_ok;
    case SignalKind::Clicks:
        return rate_ok && length_ok && signal.bpm > 0.0 && signal.bpm <= kMaxBpm;
    }
    return false;
}

/// Whether x is in [lo, hi). Written so that NaN fails.
inline bool in_half_open(double x, double lo, double hi) {
    return x >= lo && x < hi;
}

/// Whether x is in (lo, hi]. Written so that NaN fails.
inline bool in_upper_closed(double x, double lo, double hi) {
    return x > lo && x <= hi;
}

using DeckTracks = std::span<std::optional<std::size_t>, engine::kDecks>;

/// Checks one event's action, given each deck's track before it, and updates
/// the decks' tracks.
inline std::optional<std::string> broken_action(const Script& script, const Action& action,
                                                DeckTracks decks) {
    if (const auto* load = std::get_if<Load>(&action)) {
        if (load->deck >= engine::kDecks || load->track >= script.tracks.size()) {
            return "a load with a bad deck or track";
        }
        decks[load->deck] = load->track;
        return std::nullopt;
    }
    if (const auto* unload = std::get_if<Unload>(&action)) {
        if (unload->deck >= engine::kDecks) {
            return "an unload with a bad deck";
        }
        decks[unload->deck].reset();
        return std::nullopt;
    }
    if (const auto* time = std::get_if<TrackTime>(&action)) {
        const bool seek_or_cue =
            time->type == ipc::CommandType::Seek || time->type == ipc::CommandType::SetCuePoint;
        const bool seconds_ok = std::isfinite(time->seconds) && time->seconds >= 0.0;
        if (!seek_or_cue || time->deck >= engine::kDecks || !seconds_ok) {
            return "a track time with a bad type, deck, or time";
        }
        if (decks[time->deck] != time->track) {
            return "a track time on a deck without that track";
        }
        return std::nullopt;
    }
    const auto& command = std::get<engine::Command>(action);
    const std::expected<engine::Command, engine::Rejection> decoded =
        engine::decode(encode(command, 1));
    if (!decoded.has_value() || *decoded != command) {
        return "a command that doesn't survive decode()";
    }
    if (command.type == ipc::CommandType::Seek || command.type == ipc::CommandType::SetCuePoint) {
        return "a seek or cue point that isn't a track time";
    }
    return std::nullopt;
}

/// Checks that `event` comes after `previous` in run order: by time, then by
/// line.
inline bool in_run_order(const Event& previous, const Event& event) {
    return previous.time < event.time ||
           (previous.time == event.time && previous.line <= event.line);
}

} // namespace properties_detail

/// The first property `script` breaks, in words, or nothing if it keeps them
/// all.
inline std::optional<std::string> broken_property(const Script& script) {
    using namespace properties_detail;
    if (!in_upper_closed(script.end, 0.0, kMaxTime)) {
        return "the end is outside (0, kMaxTime]";
    }
    if (script.tracks.size() > kMaxTracks || script.events.size() > kMaxEvents) {
        return "too many tracks or events";
    }
    for (const TrackSpec& track : script.tracks) {
        if (const auto* signal = std::get_if<Signal>(&track.source);
            signal != nullptr && !valid_signal(*signal)) {
            return "a signal outside its limits";
        }
    }

    // Walk the events in run order, keeping each deck's track.
    std::array<std::optional<std::size_t>, engine::kDecks> deck_tracks{};
    const Event* previous = nullptr;
    for (const Event& event : script.events) {
        if (!in_half_open(event.time, 0.0, script.end) || event.line == 0) {
            return "an event outside [0, end), or without a line";
        }
        if (previous != nullptr && !in_run_order(*previous, event)) {
            return "events out of run order";
        }
        previous = &event;
        if (auto broken = broken_action(script, event.action, DeckTracks{deck_tracks})) {
            return broken;
        }
    }
    return std::nullopt;
}

/// The fuzz target's check: reading `data` as a script either fails or gives
/// a script that keeps every property. Returns false if a property breaks.
inline bool check_input(std::span<const std::uint8_t> data) {
    const std::string text(data.begin(), data.end());
    const std::expected<Script, ScriptError> script =
        read_script(text, std::filesystem::path{"/fuzz"});
    return !script.has_value() || !broken_property(*script).has_value();
}

} // namespace ks::harness::test
