// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/engine.h>

#include <expected>
#include <span>

namespace ks::engine {

Engine::Engine(const BusMap& map) : mixer_{interpolator_, map} {}

// ---- The control thread --------------------------------------------------------

// Stubs: loads and unloads come in the next task (specs/commands/plan.md,
// task 4).

// NOLINTBEGIN(readability-convert-member-functions-to-static): stubs; the
// implementation uses the engine's members.

bool Engine::load(std::size_t /*deck*/, const Track& /*track*/) noexcept {
    return false;
}

bool Engine::unload(std::size_t /*deck*/) noexcept {
    return false;
}

// NOLINTEND(readability-convert-member-functions-to-static)

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
    while (const std::optional<Handover> handover = give_consumer_.try_pop()) {
        apply_handover(*handover);
    }
    read_commands();
    mixer_.process(block);
    push_releases();
}

void Engine::apply_handover(const Handover& handover) noexcept KS_NONBLOCKING {
    // The control thread only gives client slots below kMaxClients, but the
    // give ring is checked like any other ring.
    if (handover.index >= kMaxClients) {
        return;
    }
    AudioClient& client = std::span{audio_clients_}[handover.index];
    switch (handover.kind) {
    case Handover::Kind::Attach:
        if (handover.ring != nullptr) {
            client.consumer.emplace(*handover.ring);
            client.stats = ClientStats{.attached = true};
        }
        return;
    case Handover::Kind::Detach:
        client.consumer.reset();
        client.stats.attached = false;
        queue_release(PendingRelease{.ring = handover.ring});
        return;
    case Handover::Kind::Load:
    case Handover::Kind::Unload:
        return;
    }
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

void Engine::queue_release(const PendingRelease& release) noexcept KS_NONBLOCKING {
    // Each client ring and reference slot is queued at most once, so the
    // queue never overflows; the check keeps a broken invariant harmless.
    if (pending_count_ == pending_.size()) {
        return;
    }
    std::span{pending_}[(pending_first_ + pending_count_) % pending_.size()] = release;
    ++pending_count_;
}

void Engine::push_releases() noexcept KS_NONBLOCKING {
    while (pending_count_ > 0) {
        const PendingRelease& release = std::span{pending_}[pending_first_];
        if (!retire_producer_.try_push(Released{.track = nullptr, .ring = release.ring})) {
            return;
        }
        pending_first_ = (pending_first_ + 1) % pending_.size();
        --pending_count_;
    }
}

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
