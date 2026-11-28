// SPDX-License-Identifier: GPL-3.0-or-later

#include "status_format.h"

#include <ks/client/snapshot.h>
#include <ks/ipc/state.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

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

/// A pass or fail check as requirement 19 writes it: 1 when it passes, 0
/// when it fails, `unknown` when it couldn't tell.
std::string_view check_field(const client::HealthInfo& health, std::uint64_t bit) noexcept {
    if ((health.known & bit) == 0) {
        return "unknown";
    }
    return (health.failing & bit) != 0 ? "0" : "1";
}

/// The failing checks' names, comma-separated, or `none`.
std::string failing_field(std::uint64_t failing) {
    constexpr std::array<std::pair<std::uint64_t, std::string_view>, 6> kNames{{
        {ipc::kHealthRealtime, "realtime"},
        {ipc::kHealthMemory, "memory"},
        {ipc::kHealthPowerProfile, "power-profile"},
        {ipc::kHealthPowerSource, "power-source"},
        {ipc::kHealthLoad, "load"},
        {ipc::kHealthAwake, "awake"},
    }};
    std::string names;
    for (const auto& [bit, name] : kNames) {
        if ((failing & bit) != 0) {
            if (!names.empty()) {
                names += ',';
            }
            names += name;
        }
    }
    return names.empty() ? std::string{"none"} : names;
}

std::string_view policy_name(client::Policy policy) noexcept {
    switch (policy) {
    case client::Policy::Other:
        return "other";
    case client::Policy::Fifo:
        return "fifo";
    case client::Policy::Rr:
        return "rr";
    case client::Policy::TimeConstraint:
        return "time-constraint";
    case client::Policy::Unknown:
        break;
    }
    return "unknown";
}

std::string_view process_lock_name(client::ProcessLock lock) noexcept {
    switch (lock) {
    case client::ProcessLock::Locked:
        return "locked";
    case client::ProcessLock::Failed:
        return "failed";
    case client::ProcessLock::None:
        break;
    }
    return "none";
}

std::string_view power_profile_name(client::PowerProfile profile) noexcept {
    switch (profile) {
    case client::PowerProfile::Normal:
        return "normal";
    case client::PowerProfile::Saving:
        return "saving";
    case client::PowerProfile::Unknown:
        break;
    }
    return "unknown";
}

std::string_view power_source_name(client::PowerSource source) noexcept {
    switch (source) {
    case client::PowerSource::Mains:
        return "mains";
    case client::PowerSource::Battery:
        return "battery";
    case client::PowerSource::Unknown:
        break;
    }
    return "unknown";
}

std::string_view awake_name(client::Awake awake) noexcept {
    switch (awake) {
    case client::Awake::Blocked:
        return "blocked";
    case client::Awake::NotBlocked:
        return "not-blocked";
    case client::Awake::Unknown:
        break;
    }
    return "unknown";
}

std::string limit(std::uint64_t value) {
    return value == ipc::kUnlimited ? std::string{"unlimited"} : std::to_string(value);
}

} // namespace

void format_health(const client::HealthInfo& health, std::ostream& out) {
    out << "health failing=" << failing_field(health.failing)
        << " realtime=" << check_field(health, ipc::kHealthRealtime)
        << " policy=" << policy_name(health.policy) << " priority=" << health.priority
        << " rtprio_limit=" << limit(health.rtprio_limit)
        << " memory=" << check_field(health, ipc::kHealthMemory)
        << " process_lock=" << process_lock_name(health.process_lock)
        << " memlock_limit=" << limit(health.memlock_limit)
        << " locked_bytes=" << health.locked_bytes << " unlocked_bytes=" << health.unlocked_bytes
        << " power_profile=" << power_profile_name(health.power_profile)
        << " power_source=" << power_source_name(health.power_source)
        << " peak_load=" << real(health.peak_load) << " awake=" << awake_name(health.awake) << '\n';
}

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
