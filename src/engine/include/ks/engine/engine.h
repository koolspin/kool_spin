// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The engine's graph (specs/commands/spec.md requirements 6 to 14): it reads
/// the clients' command rings at the start of each block, renders the mixer,
/// and hands tracks and rings to and from the audio thread through the give
/// and retire rings (ADR 0003 §4 and §10).

#include <ks/core/realtime.h>
#include <ks/engine/block.h>
#include <ks/engine/command.h>
#include <ks/engine/interpolator.h>
#include <ks/engine/mixer.h>
#include <ks/engine/render_core.h>
#include <ks/engine/track.h>
#include <ks/ipc/command.h>

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

    /// The mixer and its decks, for reading their state between blocks.
    [[nodiscard]] const Mixer& mixer() const noexcept KS_NONBLOCKING;

    /// Client slot `client`'s statistics, between blocks. A slot of
    /// kMaxClients or more reads as never attached.
    [[nodiscard]] ClientStats client_stats(std::size_t client) const noexcept KS_NONBLOCKING;

private:
    Interpolator interpolator_;
    Mixer mixer_;
};

} // namespace ks::engine
