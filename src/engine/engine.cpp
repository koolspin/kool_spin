// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/engine.h>

#include <expected>
#include <span>

namespace ks::engine {

Engine::Engine(const BusMap& map) : mixer_{interpolator_, map} {}

// Stub (specs/state-out/spec.md requirement 16): the state block isn't kept.
Engine::Engine(ipc::EngineState& /*state*/, const BusMap& map) : Engine{map} {}

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
    push_releases();
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

// Stub (specs/state-out/spec.md requirement 17): publishes nothing yet.
void Engine::rendered(const Block& /*block*/,
                      const RenderStats& /*stats*/) noexcept KS_NONBLOCKING {}

const Mixer& Engine::mixer() const noexcept KS_NONBLOCKING {
    return mixer_;
}

ClientStats Engine::client_stats(std::size_t client) const noexcept KS_NONBLOCKING {
    if (client >= kMaxClients) {
        return {};
    }
    return std::span{audio_clients_}[client].stats;
}

} // namespace ks::engine
