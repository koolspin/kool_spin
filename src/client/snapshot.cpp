// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/client/snapshot.h>

#include <ks/client/session.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ks::client {

std::string_view rejection_name(Rejection rejection) noexcept {
    switch (rejection) {
    case Rejection::None:
        return "none";
    case Rejection::UnknownType:
        return "unknown type";
    case Rejection::BadTarget:
        return "bad target";
    case Rejection::NonzeroPadding:
        return "nonzero padding";
    case Rejection::BadValue:
        return "bad value";
    }
    // Codes are validated before they become a Rejection (Session::snapshot_from()).
    return "none";
}

namespace {

using Clock = std::chrono::steady_clock;

// The largest valid codes (ipc/state.h).
constexpr std::uint64_t kMaxOutputState = 3;
constexpr std::uint64_t kMaxSide = 2;
constexpr std::uint64_t kMaxRejection = 4;

Error bad_state(std::string text) {
    return Error{
        .kind = ErrorKind::BadState, .code = ipc::ErrorCode::BadMessage, .text = std::move(text)};
}

/// Reads `lock` until a stable copy, or until `deadline`. Always tries at
/// least once, so a stable seqlock is read even with no time left.
template <typename T>
std::optional<T> read_until(ipc::Seqlock<T>& lock, Clock::time_point deadline) {
    const ipc::SeqlockReader<T> reader{lock};
    for (;;) {
        std::optional<T> value = reader.try_read();
        if (value.has_value() || Clock::now() >= deadline) {
            return value;
        }
        std::this_thread::yield();
    }
}

double real(std::uint64_t bits) noexcept {
    return std::bit_cast<double>(bits);
}

bool flag(std::uint64_t flags, std::uint64_t bit) noexcept {
    return (flags & bit) != 0;
}

DeckInfo deck_info(const ipc::DeckState& deck) noexcept {
    return DeckInfo{
        .loaded = flag(deck.flags, ipc::kDeckLoaded),
        .playing = flag(deck.flags, ipc::kDeckPlaying),
        .waiting = flag(deck.flags, ipc::kDeckWaiting),
        .complete = flag(deck.flags, ipc::kDeckComplete),
        .track_id = deck.track_id,
        .track_sample_rate = deck.track_sample_rate,
        .track_frames = deck.track_frames,
        .frames_ready = deck.frames_ready,
        .position = real(deck.position),
        .rate = real(deck.rate),
        .cue = real(deck.cue),
    };
}

/// Fills the engine and client parts of `snapshot` from `status`.
Result<void> convert_status(const ipc::EngineStatus& status, Snapshot& snapshot) {
    if (status.output_state > kMaxOutputState) {
        return std::unexpected{
            bad_state("invalid output_state: " + std::to_string(status.output_state))};
    }
    snapshot.engine = EngineInfo{
        .heartbeat = status.heartbeat,
        .frames_rendered = status.frames_rendered,
        .sample_rate = status.sample_rate,
        .state = static_cast<OutputState>(status.output_state),
        .muted = flag(status.flags, ipc::kStatusMuted),
        .silenced = flag(status.flags, ipc::kStatusSilenced),
        .non_finite_samples = status.non_finite_samples,
        .limited_frames = status.limited_frames,
        .late_blocks = status.late_blocks,
        .render_load = real(status.render_load),
        .peak_render_load = real(status.peak_render_load),
    };
    const std::span<ClientInfo, ipc::kStateClients> clients{snapshot.clients};
    std::size_t n = 0;
    for (const ipc::ClientAck& ack : status.clients) {
        if (ack.last_rejection > kMaxRejection) {
            return std::unexpected{
                bad_state("invalid client " + std::to_string(n) +
                          " last_rejection: " + std::to_string(ack.last_rejection))};
        }
        clients[n] = ClientInfo{
            .attached = flag(ack.flags, ipc::kClientAttached),
            .corrupt = flag(ack.flags, ipc::kClientCorrupt),
            .commands_read = ack.commands_read,
            .last_sequence = ack.last_sequence,
            .rejected = ack.rejected,
            .last_rejection = static_cast<Rejection>(ack.last_rejection),
        };
        ++n;
    }
    return {};
}

/// Fills the channel and mixer parts of `snapshot` from `mixer`.
Result<void> convert_mixer(const ipc::MixerState& mixer, Snapshot& snapshot) {
    const std::span<ChannelInfo, ipc::kStateDecks> channels{snapshot.channels};
    std::size_t n = 0;
    for (const ipc::ChannelState& channel : mixer.channels) {
        if (channel.side > kMaxSide) {
            return std::unexpected{bad_state("invalid channel " + std::to_string(n) +
                                             " side: " + std::to_string(channel.side))};
        }
        channels[n] = ChannelInfo{
            .trim_db = real(channel.trim_db),
            .fader = real(channel.fader),
            .side = static_cast<Side>(channel.side),
            .cue = channel.cue != 0,
        };
        ++n;
    }
    snapshot.mixer = MixerInfo{
        .crossfader = real(mixer.crossfader),
        .crossfader_transition = real(mixer.crossfader_transition),
        .main_level_db = real(mixer.main_level_db),
        .headphone_level_db = real(mixer.headphone_level_db),
        .cue_mix = real(mixer.cue_mix),
        .bus_main = mixer.bus_main,
        .bus_cue = mixer.bus_cue,
    };
    return {};
}

} // namespace

Result<Snapshot> Session::snapshot_from(ipc::EngineState& state, Milliseconds budget) {
    const Clock::time_point deadline = Clock::now() + budget;
    Snapshot snapshot{};

    const std::optional<ipc::EngineStatus> status = read_until(state.status, deadline);
    if (!status.has_value()) {
        return std::unexpected{bad_state("unstable seqlock: status")};
    }
    if (Result<void> converted = convert_status(*status, snapshot); !converted) {
        return std::unexpected{std::move(converted.error())};
    }

    const std::span<DeckInfo, ipc::kStateDecks> decks{snapshot.decks};
    std::size_t n = 0;
    for (ipc::Seqlock<ipc::DeckState>& lock : state.decks) {
        const std::optional<ipc::DeckState> deck = read_until(lock, deadline);
        if (!deck.has_value()) {
            return std::unexpected{bad_state("unstable seqlock: deck " + std::to_string(n))};
        }
        decks[n] = deck_info(*deck);
        ++n;
    }

    const std::optional<ipc::MixerState> mixer = read_until(state.mixer, deadline);
    if (!mixer.has_value()) {
        return std::unexpected{bad_state("unstable seqlock: mixer")};
    }
    if (Result<void> converted = convert_mixer(*mixer, snapshot); !converted) {
        return std::unexpected{std::move(converted.error())};
    }
    return snapshot;
}

} // namespace ks::client
