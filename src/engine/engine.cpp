// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/engine.h>

#include "codes.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <span>

namespace ks::engine {

namespace {

// The state segment's encodings (specs/state-out/spec.md requirement 1). The
// ones the resume slots share are in codes.h.

using codes::flag_code;
using codes::host_time_code;
using codes::real_code;
using codes::side_code;

std::uint64_t duration_code(std::chrono::nanoseconds duration) noexcept KS_NONBLOCKING {
    return static_cast<std::uint64_t>(duration.count());
}

std::uint64_t output_state_code(OutputState state) noexcept KS_NONBLOCKING {
    switch (state) {
    case OutputState::Stopped:
        return 0;
    case OutputState::Playing:
        return 1;
    case OutputState::Freewheeling:
        return 2;
    case OutputState::Attaching:
        return 3;
    }
    return 0;
}

/// 0 for none, then the Rejection's value plus 1.
std::uint64_t rejection_code(const std::optional<Rejection>& rejection) noexcept KS_NONBLOCKING {
    if (!rejection.has_value()) {
        return 0;
    }
    switch (*rejection) {
    case Rejection::UnknownType:
        return 1;
    case Rejection::BadTarget:
        return 2;
    case Rejection::NonzeroPadding:
        return 3;
    case Rejection::BadValue:
        return 4;
    }
    return 0;
}

} // namespace

Engine::Engine(const BusMap& map) : mixer_{interpolator_, map} {}

Engine::Engine(ipc::EngineState& state, const BusMap& map) : Engine{map} {
    // The only reads of the state block: each writer continues from its
    // sequence number (requirement 16).
    for (std::size_t i = 0; i < kDecks; ++i) {
        deck_writers_.at(i).emplace(state.decks.at(i));
    }
    mixer_writer_.emplace(state.mixer);
    status_writer_.emplace(state.status);
    std::size_t slot = 0;
    for (ipc::Seqlock<ipc::ResumeState>& lock : state.resume) {
        std::span{resume_writers_}[slot].emplace(lock);
        ++slot;
    }
    // Generations carry on from the newest valid snapshot
    // (specs/resume-state/spec.md requirement 12). A valid generation is at
    // most kMaxGeneration, so the next one saturates there rather than
    // becoming invalid.
    found_snapshot_ = read_snapshot(state);
    if (found_snapshot_.has_value()) {
        next_generation_ = found_snapshot_->generation < kMaxGeneration
                               ? found_snapshot_->generation + 1
                               : kMaxGeneration;
    }
}

Engine::Engine(ipc::EngineState& state, std::span<const Track* const> tracks, const BusMap& map)
    : Engine{state, map} {
    // Requirements 13 and 14. This runs before any block, on the control
    // thread, so the audio thread's members are free to set here.
    if (!found_snapshot_.has_value()) {
        return;
    }
    const Snapshot& snapshot = *found_snapshot_;
    // Every value in a valid snapshot is in range, so the setters keep it
    // as it is, and settings() equals the snapshot's.
    const MixerSettings& settings = snapshot.mixer;
    for (std::size_t i = 0; i < kDecks; ++i) {
        const ChannelSettings& channel = std::span{settings.channels}[i];
        mixer_.set_trim(i, channel.trim_db);
        mixer_.set_fader(i, channel.fader);
        mixer_.set_crossfader_assign(i, channel.side);
        mixer_.set_cue(i, channel.cue);
    }
    mixer_.set_crossfader(settings.crossfader);
    mixer_.set_crossfader_curve(settings.crossfader_transition);
    mixer_.set_main_level(settings.main_level_db);
    mixer_.set_headphone_level(settings.headphone_level_db);
    mixer_.set_cue_mix(settings.cue_mix);
    mixer_.set_bus_map(settings.bus_map);

    for (std::size_t i = 0; i < kDecks; ++i) {
        const DeckSnapshot& saved = std::span{snapshot.decks}[i];
        Deck& deck = mixer_.deck(i);
        if (saved.track_id != 0) {
            // The first non-null track with the ID; ID 0 never matches.
            const Track* found = nullptr;
            for (const Track* const track : tracks) {
                if (track != nullptr && track->id() == saved.track_id) {
                    found = track;
                    break;
                }
            }
            // The deck is silent and the engine holds nothing yet, so the
            // load is immediate and the deck's first place is free; the
            // reference is kept there, as apply_load() keeps one.
            if (found != nullptr && deck.load(*found)) {
                deck.set_cue(saved.cue);
                std::span{places_}[kPlacesPerDeck * i] = found;
                std::span{resume_outcome_.tracks}[i] = found;
            }
        }
        deck.set_rate(saved.rate);
    }
    resume_outcome_.generation = snapshot.generation;
    // The first rendered block moves the resumed decks on and starts the
    // fade-in (requirements 15 and 16).
    resume_pending_ = true;
}

// ---- The control thread --------------------------------------------------------

bool Engine::load(std::size_t deck, const Track& track) noexcept {
    if (deck >= kDecks) {
        return false;
    }
    return give(
        Handover{.kind = Handover::Kind::Load, .index = deck, .track = &track, .ring = nullptr});
}

bool Engine::unload(std::size_t deck) noexcept {
    if (deck >= kDecks) {
        return false;
    }
    return give(
        Handover{.kind = Handover::Kind::Unload, .index = deck, .track = nullptr, .ring = nullptr});
}

std::optional<std::size_t> Engine::attach(ipc::CommandRing& ring) noexcept {
    for (std::size_t i = 0; i < kMaxClients; ++i) {
        ControlClient& client = control_clients_.at(i);
        if (client.ring != nullptr) {
            continue;
        }
        if (!give(Handover{
                .kind = Handover::Kind::Attach, .index = i, .track = nullptr, .ring = &ring})) {
            return std::nullopt;
        }
        client = ControlClient{.ring = &ring, .detaching = false};
        return i;
    }
    return std::nullopt;
}

bool Engine::detach(std::size_t client) noexcept {
    if (client >= kMaxClients) {
        return false;
    }
    ControlClient& slot = control_clients_.at(client);
    if (slot.ring == nullptr || slot.detaching) {
        return false;
    }
    if (!give(Handover{.kind = Handover::Kind::Detach,
                       .index = client,
                       .track = nullptr,
                       .ring = slot.ring})) {
        return false;
    }
    slot.detaching = true;
    return true;
}

std::optional<Released> Engine::collect() noexcept {
    const std::optional<Released> released = retire_consumer_.try_pop();
    if (released.has_value() && released->ring != nullptr) {
        for (ControlClient& client : control_clients_) {
            if (client.ring == released->ring && client.detaching) {
                client = ControlClient{};
                break;
            }
        }
    }
    return released;
}

bool Engine::give(const Handover& handover) noexcept {
    return give_producer_.try_push(handover);
}

// ---- The audio thread ----------------------------------------------------------

void Engine::process(const Block& block) noexcept KS_NONBLOCKING {
    if (block.frames == 0 || block.frames > kMaxBlockFrames || block.sample_rate == 0) {
        return;
    }
    if (resume_pending_) {
        resume_pending_ = false;
        resume_moves(block);
        // Requirement 16: N is kResumeFade at this block's sample rate,
        // rounded to the nearest frame. Integer arithmetic gives the exact
        // rounding; a rate too low for one frame has no fade.
        constexpr auto kFadeUnits = static_cast<std::uint64_t>(kResumeFade.count());
        constexpr std::uint64_t kUnitsPerSecond = decltype(kResumeFade)::period::den;
        fade_frames_ = ((kFadeUnits * block.sample_rate) + (kUnitsPerSecond / 2)) / kUnitsPerSecond;
        fade_frame_ = 0;
        fading_ = fade_frames_ > 0;
    }

    // A stalled handover goes first, and while it waits nothing behind it applies
    // (requirement 13).
    if (!stalled_.has_value() || apply_handover(*stalled_)) {
        stalled_.reset();
        while (const std::optional<Handover> handover = give_consumer_.try_pop()) {
            if (!apply_handover(*handover)) {
                stalled_ = *handover;
                break;
            }
        }
    }
    read_commands();
    mixer_.process(block);
    if (fading_) {
        apply_fade(block);
    }
    push_releases();
}

void Engine::apply_fade(const Block& block) noexcept KS_NONBLOCKING {
    // Requirement 16: frame n, counted from the first rendered block, has
    // gain n / N, in double; from frame N on the gain is 1. The count carries
    // across blocks, so the output doesn't depend on block sizes.
    const auto total = static_cast<double>(fade_frames_);
    for (std::size_t f = 0; f < block.frames; ++f) {
        const std::uint64_t n = fade_frame_ + f;
        if (n >= fade_frames_) {
            break;
        }
        const double gain = static_cast<double>(n) / total;
        for (const std::span<float> channel : block.channels) {
            float& sample = channel[f];
            sample = static_cast<float>(static_cast<double>(sample) * gain);
        }
    }
    fade_frame_ += block.frames;
    if (fade_frame_ >= fade_frames_) {
        fading_ = false;
    }
}

void Engine::resume_moves(const Block& block) noexcept KS_NONBLOCKING {
    // Requirement 15: each resumed deck goes to the snapshot's position,
    // moved on by the time since it was written if it was playing. The deck
    // clamps the result to its track.
    if (!found_snapshot_.has_value()) {
        return;
    }
    const Snapshot& snapshot = *found_snapshot_;
    const std::chrono::steady_clock::duration gap = block.host_time - snapshot.host_time;
    const double elapsed = gap.count() > 0 ? std::chrono::duration<double>(gap).count() : 0.0;
    for (std::size_t i = 0; i < kDecks; ++i) {
        const Track* const track = std::span{resume_outcome_.tracks}[i];
        if (track == nullptr) {
            continue;
        }
        const DeckSnapshot& saved = std::span{snapshot.decks}[i];
        Deck& deck = mixer_.deck(i);
        const double travel =
            saved.playing ? saved.rate * static_cast<double>(track->sample_rate()) * elapsed : 0.0;
        deck.seek(saved.position + travel);
        if (saved.playing) {
            deck.set_playing(true);
        }
    }
}

bool Engine::apply_handover(const Handover& handover) noexcept KS_NONBLOCKING {
    // The control thread only gives valid indices, but the give ring is
    // checked like any other ring.
    switch (handover.kind) {
    case Handover::Kind::Load:
        if (handover.index >= kDecks || handover.track == nullptr) {
            return true;
        }
        return apply_load(handover.index, *handover.track);
    case Handover::Kind::Unload:
        if (handover.index < kDecks) {
            if (release_room() < kPlacesPerDeck) {
                return false;
            }
            mixer_.deck(handover.index).unload();
            let_go(handover.index);
        }
        return true;
    case Handover::Kind::Attach:
    case Handover::Kind::Detach:
        break;
    }
    if (handover.index >= kMaxClients) {
        return true;
    }
    AudioClient& client = std::span{audio_clients_}[handover.index];
    if (handover.kind == Handover::Kind::Attach) {
        if (handover.ring != nullptr) {
            client.consumer.emplace(*handover.ring);
            client.stats = ClientStats{.attached = true};
        }
        return true;
    }
    if (release_room() == 0) {
        return false;
    }
    client.consumer.reset();
    client.stats.attached = false;
    static_cast<void>(
        queue_release(PendingRelease{.track = nullptr, .ring = handover.ring, .count = 0}));
    return true;
}

bool Engine::apply_load(std::size_t deck, const Track& track) noexcept KS_NONBLOCKING {
    // Room for a duplicate, plus every place's reference (requirement 13).
    if (release_room() < kPlacesPerDeck + 1) {
        return false;
    }
    // The places then hold only tracks the deck holds: at most 3.
    let_go(deck);
    const std::span<const Track*> places =
        std::span{places_}.subspan(deck * kPlacesPerDeck, kPlacesPerDeck);
    bool placed = false;
    for (const Track* const place : places) {
        if (place == &track) {
            // A duplicate: the older reference is released in this block's
            // releases, and the newer one takes its place (requirement 11).
            static_cast<void>(
                queue_release(PendingRelease{.track = &track, .ring = nullptr, .count = 1}));
            placed = true;
            break;
        }
    }
    for (std::size_t i = 0; i < places.size() && !placed; ++i) {
        if (places[i] == nullptr) {
            places[i] = &track;
            placed = true;
        }
    }
    if (!placed) {
        // Can't happen while the deck holds at most 3 tracks; waiting keeps
        // the reference rather than losing it.
        return false;
    }
    // A refused load, or a waiting load this one replaced, leaves at once.
    static_cast<void>(mixer_.deck(deck).load(track));
    let_go(deck);
    return true;
}

void Engine::read_commands() noexcept KS_NONBLOCKING {
    for (AudioClient& client : audio_clients_) {
        if (!client.consumer.has_value() || client.stats.corrupt) {
            continue;
        }
        CommandConsumer& consumer = *client.consumer;
        for (std::size_t n = 0; n < kCommandsPerBlock; ++n) {
            const std::optional<ipc::CommandSlot> slot = consumer.try_pop();
            if (!slot.has_value()) {
                break;
            }
            ++client.stats.commands_read;
            client.stats.last_sequence = slot->sequence;
            const std::expected<Command, Rejection> command = decode(*slot);
            if (command.has_value()) {
                apply(*command, mixer_);
            } else {
                ++client.stats.rejected;
                client.stats.last_rejection = command.error();
            }
        }
        if (consumer.corrupt()) {
            client.stats.corrupt = true;
        }
    }
}

std::size_t Engine::release_room() const noexcept KS_NONBLOCKING {
    return pending_.size() - pending_count_;
}

bool Engine::queue_release(const PendingRelease& release) noexcept KS_NONBLOCKING {
    if (release.track != nullptr && pending_count_ > 0) {
        PendingRelease& last =
            std::span{pending_}[(pending_first_ + pending_count_ - 1) % pending_.size()];
        if (last.track == release.track) {
            last.count += release.count;
            return true;
        }
    }
    if (pending_count_ == pending_.size()) {
        return false;
    }
    std::span{pending_}[(pending_first_ + pending_count_) % pending_.size()] = release;
    ++pending_count_;
    return true;
}

void Engine::let_go(std::size_t deck) noexcept KS_NONBLOCKING {
    const Deck& owner = mixer_.deck(deck);
    for (const Track*& place : std::span{places_}.subspan(deck * kPlacesPerDeck, kPlacesPerDeck)) {
        // With the queue full, the reference stays in its place until there's
        // room.
        if (place != nullptr && !owner.holds(*place) &&
            queue_release(PendingRelease{.track = place, .ring = nullptr, .count = 1})) {
            place = nullptr;
        }
    }
}

void Engine::push_releases() noexcept KS_NONBLOCKING {
    // This catches fades that finished while mixing (requirement 11).
    for (std::size_t deck = 0; deck < kDecks; ++deck) {
        let_go(deck);
    }
    while (pending_count_ > 0) {
        if (!drain(std::span{pending_}[pending_first_])) {
            return;
        }
        pending_first_ = (pending_first_ + 1) % pending_.size();
        --pending_count_;
    }
}

bool Engine::drain(PendingRelease& release) noexcept KS_NONBLOCKING {
    if (release.ring != nullptr) {
        return retire_producer_.try_push(Released{.track = nullptr, .ring = release.ring});
    }
    // A partly drained entry keeps its place at the front of the queue.
    for (; release.count > 0; --release.count) {
        if (!retire_producer_.try_push(Released{.track = release.track, .ring = nullptr})) {
            return false;
        }
    }
    return true;
}

void Engine::rendered(const Block& block, const RenderStats& stats) noexcept KS_NONBLOCKING {
    if (!status_writer_.has_value() || !mixer_writer_.has_value()) {
        return;
    }
    const std::uint64_t heartbeat = stats.blocks_rendered;
    // The same expression as the render core's rendered_until().
    const std::uint64_t end =
        host_time_code(block.host_time + frames_to_duration(block.frames, block.sample_rate));
    const std::uint64_t latency = duration_code(block.output_latency);

    // Requirement 3.
    for (std::size_t i = 0; i < kDecks; ++i) {
        std::optional<ipc::SeqlockWriter<ipc::DeckState>>& writer = std::span{deck_writers_}[i];
        if (!writer.has_value()) {
            continue;
        }
        const Deck& deck = mixer_.deck(i);
        ipc::DeckState state{};
        state.heartbeat = heartbeat;
        state.flags =
            (deck.playing() ? ipc::kDeckPlaying : 0U) | (deck.waiting() ? ipc::kDeckWaiting : 0U);
        if (const Track* const track = deck.track(); track != nullptr) {
            const TrackReady ready = track->ready();
            state.flags |= ipc::kDeckLoaded | (ready.complete ? ipc::kDeckComplete : 0U);
            state.track_id = track->id();
            state.track_sample_rate = track->sample_rate();
            state.track_frames = track->capacity();
            state.frames_ready = ready.frames;
        }
        state.position = real_code(deck.position());
        state.rate = real_code(deck.rate());
        state.cue = real_code(deck.cue());
        state.host_time = end;
        state.output_latency = latency;
        writer->write(state);
    }

    // Requirement 4.
    const MixerSettings& settings = mixer_.settings();
    ipc::MixerState mixer{};
    mixer.heartbeat = heartbeat;
    for (std::size_t i = 0; i < kDecks; ++i) {
        const ChannelSettings& channel = std::span{settings.channels}[i];
        ipc::ChannelState& out = std::span{mixer.channels}[i];
        out.trim_db = real_code(channel.trim_db);
        out.fader = real_code(channel.fader);
        out.side = side_code(channel.side);
        out.cue = flag_code(channel.cue);
    }
    mixer.crossfader = real_code(settings.crossfader);
    mixer.crossfader_transition = real_code(settings.crossfader_transition);
    mixer.main_level_db = real_code(settings.main_level_db);
    mixer.headphone_level_db = real_code(settings.headphone_level_db);
    mixer.cue_mix = real_code(settings.cue_mix);
    mixer.bus_main = settings.bus_map.main;
    mixer.bus_cue = settings.bus_map.cue;
    mixer_writer_->write(mixer);

    // Requirements 5 and 6.
    ipc::EngineStatus status{};
    for (std::size_t i = 0; i < kMaxClients; ++i) {
        const ClientStats& client = std::span{audio_clients_}[i].stats;
        ipc::ClientAck& ack = std::span{status.clients}[i];
        ack.flags = (client.attached ? ipc::kClientAttached : 0U) |
                    (client.corrupt ? ipc::kClientCorrupt : 0U);
        ack.commands_read = client.commands_read;
        ack.last_sequence = client.last_sequence;
        ack.rejected = client.rejected;
        ack.last_rejection = rejection_code(client.last_rejection);
    }
    status.heartbeat = heartbeat;
    status.frames_rendered = stats.frames_rendered;
    status.sample_rate = block.sample_rate;
    status.host_time = end;
    status.output_latency = latency;
    status.output_state = output_state_code(stats.output_state);
    status.flags =
        (stats.muted ? ipc::kStatusMuted : 0U) | (stats.silenced ? ipc::kStatusSilenced : 0U);
    status.non_finite_samples = stats.non_finite_samples;
    status.limited_frames = stats.limited_frames;
    status.late_blocks = stats.late_blocks;
    status.render_time = duration_code(stats.render_time);
    status.render_load = real_code(stats.render_load);
    status.peak_render_load = real_code(stats.peak_render_load);
    status_writer_->write(status);

    write_snapshot(block);
}

void Engine::write_snapshot(const Block& block) noexcept KS_NONBLOCKING {
    std::optional<ipc::SeqlockWriter<ipc::ResumeState>>& writer =
        std::span{resume_writers_}[next_generation_ % ipc::kResumeSlots];
    if (!writer.has_value()) {
        return;
    }
    Snapshot snapshot{};
    snapshot.generation = next_generation_;
    // The same expression as the status block's host time.
    snapshot.host_time = block.host_time + frames_to_duration(block.frames, block.sample_rate);
    for (std::size_t i = 0; i < kDecks; ++i) {
        const Deck& deck = mixer_.deck(i);
        DeckSnapshot& out = std::span{snapshot.decks}[i];
        // A track with ID 0 counts as no track: no engine could resume it.
        if (const Track* const track = deck.track(); track != nullptr && track->id() != 0) {
            out.track_id = track->id();
            out.playing = deck.playing();
            out.position = deck.position();
            out.cue = deck.cue();
        }
        out.rate = deck.rate();
    }
    snapshot.mixer = mixer_.settings();
    writer->write(encode(snapshot));
    // Saturates at the highest valid generation; unreachable in practice.
    if (next_generation_ < kMaxGeneration) {
        ++next_generation_;
    }
}

const Mixer& Engine::mixer() const noexcept KS_NONBLOCKING {
    return mixer_;
}

const ResumeOutcome& Engine::resume_outcome() const noexcept KS_NONBLOCKING {
    return resume_outcome_;
}

ClientStats Engine::client_stats(std::size_t client) const noexcept KS_NONBLOCKING {
    if (client >= kMaxClients) {
        return {};
    }
    return std::span{audio_clients_}[client].stats;
}

} // namespace ks::engine
