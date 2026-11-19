// SPDX-License-Identifier: GPL-3.0-or-later

#include "status_format.h"

#include <ks/client/snapshot.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <ostream>
#include <string>
#include <string_view>

namespace ks::play_app {
namespace {

/// A real as `%.6g`: NaN and the infinities come out as `nan`, `inf`, `-inf`.
std::string real(double value) {
    std::array<char, 64> buffer{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): requirement 25 asks for %.6g exactly
    const int written = std::snprintf(buffer.data(), buffer.size(), "%.6g", value);
    if (written < 0) {
        return "nan";
    }
    return std::string{buffer.data()};
}

char flag(bool value) noexcept {
    return value ? '1' : '0';
}

std::string_view state_name(client::OutputState state) noexcept {
    switch (state) {
    case client::OutputState::Stopped:
        return "stopped";
    case client::OutputState::Playing:
        return "playing";
    case client::OutputState::Freewheeling:
        return "freewheeling";
    case client::OutputState::Attaching:
        return "attaching";
    }
    return "stopped";
}

std::string_view side_name(client::Side side) noexcept {
    switch (side) {
    case client::Side::A:
        return "a";
    case client::Side::B:
        return "b";
    case client::Side::Thru:
        return "thru";
    }
    return "thru";
}

/// A rejection's name with its spaces written `_`, so it's one field.
std::string rejection_field(client::Rejection rejection) {
    std::string name{client::rejection_name(rejection)};
    for (char& c : name) {
        if (c == ' ') {
            c = '_';
        }
    }
    return name;
}

} // namespace

void format_status(const client::Snapshot& snapshot, std::ostream& out) {
    const client::EngineInfo& engine = snapshot.engine;
    out << "engine heartbeat=" << engine.heartbeat << " state=" << state_name(engine.state)
        << " sample_rate=" << engine.sample_rate << " muted=" << flag(engine.muted)
        << " silenced=" << flag(engine.silenced) << " late_blocks=" << engine.late_blocks
        << " non_finite_samples=" << engine.non_finite_samples
        << " limited_frames=" << engine.limited_frames
        << " render_load=" << real(engine.render_load)
        << " peak_render_load=" << real(engine.peak_render_load) << '\n';

    std::size_t n = 0;
    for (const client::DeckInfo& deck : snapshot.decks) {
        out << "deck " << n++ << " loaded=" << flag(deck.loaded)
            << " playing=" << flag(deck.playing) << " waiting=" << flag(deck.waiting)
            << " complete=" << flag(deck.complete) << " track=" << deck.track_id
            << " track_rate=" << deck.track_sample_rate << " track_frames=" << deck.track_frames
            << " frames_ready=" << deck.frames_ready << " position=" << real(deck.position)
            << " rate=" << real(deck.rate) << " cue=" << real(deck.cue) << '\n';
    }

    n = 0;
    for (const client::ChannelInfo& channel : snapshot.channels) {
        out << "channel " << n++ << " trim_db=" << real(channel.trim_db)
            << " fader=" << real(channel.fader) << " side=" << side_name(channel.side)
            << " cue=" << flag(channel.cue) << '\n';
    }

    const client::MixerInfo& mixer = snapshot.mixer;
    out << "mixer crossfader=" << real(mixer.crossfader)
        << " curve=" << real(mixer.crossfader_transition)
        << " main_db=" << real(mixer.main_level_db)
        << " headphone_db=" << real(mixer.headphone_level_db) << " cue_mix=" << real(mixer.cue_mix)
        << " bus_main=" << mixer.bus_main << " bus_cue=" << mixer.bus_cue << '\n';

    n = 0;
    for (const client::ClientInfo& slot : snapshot.clients) {
        if (slot.attached || slot.corrupt) {
            out << "client " << n << " corrupt=" << flag(slot.corrupt)
                << " commands_read=" << slot.commands_read
                << " last_sequence=" << slot.last_sequence << " rejected=" << slot.rejected
                << " last_rejection=" << rejection_field(slot.last_rejection) << '\n';
        }
        ++n;
    }
}

} // namespace ks::play_app
