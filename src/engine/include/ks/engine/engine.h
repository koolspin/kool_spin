// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The engine's graph (specs/commands/spec.md requirements 6 to 14): it reads
/// the clients' command rings at the start of each block, renders the mixer,
/// and hands tracks and rings to and from the audio thread through the give
/// and retire rings (ADR 0003 §4 and §10). After each block the render core
/// renders, it publishes its state (specs/state-out/spec.md requirements 16
/// to 20; ADR 0003 §5).

#include <ks/core/realtime.h>
#include <ks/engine/block.h>
#include <ks/engine/command.h>
#include <ks/engine/interpolator.h>
#include <ks/engine/mixer.h>
#include <ks/engine/render_core.h>
#include <ks/engine/track.h>
#include <ks/ipc/command.h>
#include <ks/ipc/spsc_ring.h>
#include <ks/ipc/state.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ks::engine {

/// The most client rings the engine reads.
inline constexpr std::size_t kMaxClients = 8;
/// The most commands the engine reads from one ring in one block.
inline constexpr std::size_t kCommandsPerBlock = 32;
/// The capacity of the give ring, and of the retire ring.
inline constexpr std::size_t kHandoverCapacity = 64;

static_assert(ipc::kStateDecks == kDecks, "the state segment describes every deck");
static_assert(ipc::kStateClients == kMaxClients, "the state segment describes every client slot");

/// What the engine knows about one client slot (spec requirement 9). A slot's
/// statistics start from zero when a client is attached to it.
struct ClientStats {
    bool attached = false;
    /// The ring was found corrupt, and isn't read any more.
    bool corrupt = false;
    /// Slots read, valid or not.
    std::uint64_t commands_read = 0;
    /// The sequence number of the last slot read; 0 before the first.
    std::uint64_t last_sequence = 0;
    /// Slots that decode() rejected.
    std::uint64_t rejected = 0;
    /// The reason for the last rejection; empty before the first.
    std::optional<Rejection> last_rejection = std::nullopt;

    bool operator==(const ClientStats&) const = default;
};

/// Something the audio thread has finished with: one reference to a track,
/// or a detached client's ring. Exactly one of the two is set.
struct Released {
    const Track* track = nullptr;
    ipc::CommandRing* ring = nullptr;
};

/// The graph the render core runs: four decks and the mixer, controlled
/// through client command rings.
///
/// Two threads use it. The audio thread calls process(), and may call
/// mixer() and client_stats() between blocks. One control thread at a time
/// calls load(), unload(), attach(), detach(), and collect(); they never
/// wait for the audio thread.
class Engine final : public Graph {
public:
    /// Builds the interpolator and the mixer, with `map` if it's valid and the
    /// default map if not. Everything the audio thread uses is allocated by
    /// the time it returns.
    explicit Engine(const BusMap& map = {});

    /// Builds the engine as the other constructor does, and publishes its
    /// state to `state` after every block the render core renders. The state
    /// block must outlive the engine. The engine writes it only from the
    /// audio thread, in rendered().
    explicit Engine(ipc::EngineState& state, const BusMap& map = {});

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;
    ~Engine() = default;

    // The control thread (requirements 7, 10, 12, and 13).

    /// Asks for `track` to be loaded on `deck`, giving the engine one
    /// reference to it. Returns false, and gives nothing, if `deck` is
    /// kDecks or more or the give ring is full. The track must stay alive
    /// until every reference given has been released through collect().
    [[nodiscard]] bool load(std::size_t deck, const Track& track) noexcept;

    /// Asks for `deck` to be unloaded. Returns false, and gives nothing, if
    /// `deck` is kDecks or more or the give ring is full.
    [[nodiscard]] bool unload(std::size_t deck) noexcept;

    /// Asks the engine to start reading `ring` from its current tail, in the
    /// lowest free client slot. Returns the slot, or nothing if all
    /// kMaxClients slots are in use or the give ring is full. A slot is in use
    /// from attach() until its ring's release has been collected. The ring
    /// must stay alive until then.
    [[nodiscard]] std::optional<std::size_t> attach(ipc::CommandRing& ring) noexcept;

    /// Asks the engine to stop reading client slot `client`'s ring. Returns
    /// false, and gives nothing, if the slot isn't attached or the give ring
    /// is full.
    [[nodiscard]] bool detach(std::size_t client) noexcept;

    /// Pops one release from the retire ring, or returns nothing if it's
    /// empty.
    [[nodiscard]] std::optional<Released> collect() noexcept;

    // The audio thread.

    /// Applies handovers, reads commands, renders the mixer, and releases
    /// what's no longer used, in that order (requirement 8).
    void process(const Block& block) noexcept KS_NONBLOCKING override;

    /// Publishes each deck's block, then the mixer's, then the status block,
    /// from what the engine holds after the block, if it was built with a
    /// state block (specs/state-out/spec.md requirement 17).
    void rendered(const Block& block, const RenderStats& stats) noexcept KS_NONBLOCKING override;

    /// The mixer and its decks, for reading their state between blocks.
    [[nodiscard]] const Mixer& mixer() const noexcept KS_NONBLOCKING;

    /// Client slot `client`'s statistics, between blocks. A slot of
    /// kMaxClients or more reads as never attached.
    [[nodiscard]] ClientStats client_stats(std::size_t client) const noexcept KS_NONBLOCKING;

private:
    /// One item on the give ring, from the control thread to the audio
    /// thread. `index` is the deck or the client slot.
    struct Handover {
        enum class Kind : std::uint8_t { Load, Unload, Attach, Detach };

        Kind kind = Kind::Load;
        std::size_t index = 0;
        const Track* track = nullptr;
        ipc::CommandRing* ring = nullptr;
    };

    using CommandConsumer = ipc::RingConsumer<ipc::CommandSlot, ipc::kCommandRingCapacity>;

    /// A client slot as the control thread sees it.
    struct ControlClient {
        /// The ring given to attach(); null while the slot is free.
        ipc::CommandRing* ring = nullptr;
        /// detach() has been given, and the release not yet collected.
        bool detaching = false;
    };

    /// A client slot as the audio thread sees it.
    struct AudioClient {
        /// Set while the client is attached.
        std::optional<CommandConsumer> consumer;
        ClientStats stats;
    };

    /// A release waiting to be pushed to the retire ring: a detached client's
    /// ring if `ring` is set, and otherwise `count` references to `track`.
    struct PendingRelease {
        const Track* track = nullptr;
        ipc::CommandRing* ring = nullptr;
        /// References to `track` still to be pushed.
        std::uint64_t count = 0;
    };

    /// Each deck has 4 places for references (requirement 11). A place holds
    /// one reference to a track the deck may still hold, so at most 3 are in
    /// use between handovers, and a load always finds a free one.
    static constexpr std::size_t kPlacesPerDeck = 4;
    static constexpr std::size_t kPlaces = kDecks * kPlacesPerDeck;

    /// The release queue's capacity. Starting a block empty, it can't fill
    /// during that block: every place's reference, one entry for each
    /// handover the give ring and stalled_ hold, and every client ring fit.
    static constexpr std::size_t kMaxPendingReleases = 128;
    static_assert(kMaxPendingReleases >= kPlaces + kHandoverCapacity + 1 + kMaxClients);

    /// Applies one handover. Returns false if it has to wait: the release
    /// queue lacks room for what it could add (requirement 13).
    [[nodiscard]] bool apply_handover(const Handover& handover) noexcept KS_NONBLOCKING;
    [[nodiscard]] bool apply_load(std::size_t deck, const Track& track) noexcept KS_NONBLOCKING;
    void read_commands() noexcept KS_NONBLOCKING;
    /// Free entries in the release queue.
    [[nodiscard]] std::size_t release_room() const noexcept KS_NONBLOCKING;
    /// Appends a release to the queue, merging one reference into the last
    /// entry if it's for the same track. Returns false if the queue is full.
    [[nodiscard]] bool queue_release(const PendingRelease& release) noexcept KS_NONBLOCKING;
    /// Moves each reference in `deck`'s places that the deck no longer holds
    /// into the release queue, as long as the queue has room.
    void let_go(std::size_t deck) noexcept KS_NONBLOCKING;
    /// Runs let_go() for every deck, then pushes queued releases until the
    /// retire ring is full.
    void push_releases() noexcept KS_NONBLOCKING;
    /// Pushes one pending release, as far as the retire ring has room.
    /// Returns false if part of it is still waiting.
    [[nodiscard]] bool drain(PendingRelease& release) noexcept KS_NONBLOCKING;
    /// Pushes `handover` to the give ring. Control thread only.
    [[nodiscard]] bool give(const Handover& handover) noexcept;

    Interpolator interpolator_;
    Mixer mixer_;

    // The two rings, in private memory: the only state both threads touch.
    ipc::SpscRing<Handover, kHandoverCapacity> give_ring_{};
    ipc::SpscRing<Released, kHandoverCapacity> retire_ring_{};

    // Control thread.
    ipc::RingProducer<Handover, kHandoverCapacity> give_producer_{give_ring_};
    ipc::RingConsumer<Released, kHandoverCapacity> retire_consumer_{retire_ring_};
    std::array<ControlClient, kMaxClients> control_clients_{};

    // Audio thread.
    ipc::RingConsumer<Handover, kHandoverCapacity> give_consumer_{give_ring_};
    ipc::RingProducer<Released, kHandoverCapacity> retire_producer_{retire_ring_};
    std::array<AudioClient, kMaxClients> audio_clients_{};
    /// Deck d's places are kPlacesPerDeck * d onwards; null when free.
    std::array<const Track*, kPlaces> places_{};
    /// A handover that has to wait for room in the release queue; it's
    /// applied before anything else is taken from the give ring.
    std::optional<Handover> stalled_;
    /// A FIFO of releases waiting for room in the retire ring.
    std::array<PendingRelease, kMaxPendingReleases> pending_{};
    std::size_t pending_first_ = 0;
    std::size_t pending_count_ = 0;
};

} // namespace ks::engine
