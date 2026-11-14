// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The player both modes share (specs/harness/spec.md requirements 18 to 20
/// and 27): the engine with its state block, the harness's command ring, and
/// the script's events, given in run order as they fall due. Everything here
/// runs on the control thread; the engine's audio thread only renders.

#include <ks/engine/engine.h>
#include <ks/harness/run.h>
#include <ks/harness/script.h>
#include <ks/harness/tracks.h>
#include <ks/ipc/command.h>
#include <ks/ipc/spsc_ring.h>
#include <ks/ipc/state.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace ks::harness {

/// The frame an event at `seconds` runs at, at `sample_rate`: round(seconds ×
/// sample_rate) (requirement 24). `seconds` is a script time: finite, at
/// least 0, and at most kMaxTime.
[[nodiscard]] std::uint64_t event_frame(double seconds, std::uint32_t sample_rate) noexcept;

/// Gives a script's events to an engine it owns.
class Player {
public:
    /// Builds the state block and the engine on it, and attaches the command
    /// ring as the engine's one client. `script` and `tracks` must outlive the
    /// player, and `tracks` must be prepared from `script`. Fails if they
    /// don't match.
    [[nodiscard]] static std::expected<std::unique_ptr<Player>, std::string>
    create(const Script& script, const TrackSet& tracks);

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    Player(Player&&) = delete;
    Player& operator=(Player&&) = delete;
    ~Player() = default;

    /// The engine, for a render core or an output controller to run.
    [[nodiscard]] engine::Engine& engine() noexcept;

    /// The time of the next event not yet given, or nothing once every event
    /// has been given.
    [[nodiscard]] std::optional<double> next_time() const noexcept;

    /// Gives, in run order, every event whose time is at most `now` seconds
    /// (device mode). Stops at the first event whose ring is full: it and
    /// every later event wait for the next call, and each event due by now
    /// that has to wait is counted as delayed once (requirement 19).
    void give_due(double now);

    /// As give_due(), for an event being due once `frame` frames at
    /// `sample_rate` have been rendered: event_frame(time, sample_rate) is at
    /// most `frame` (file mode).
    void give_due_at_frame(std::uint64_t frame, std::uint32_t sample_rate);

    /// Collects every release the engine has made. The tracks stay alive in
    /// the track set, so there is nothing to free (requirement 20).
    void collect() noexcept;

    /// Whether every command pushed has been read by the engine, as its
    /// status block reports.
    [[nodiscard]] bool all_read();

    /// The report's command counts, and what the engine's status block says
    /// (requirement 27): the played time (frames rendered ÷ rate), blocks
    /// rendered, late blocks, peak render load, non-finite samples, limited
    /// frames, and commands read and rejected. stopped_early and
    /// device_losses are left as they were.
    void fill(Report& report);

    [[nodiscard]] std::uint64_t commands_sent() const noexcept;
    [[nodiscard]] std::uint64_t events_delayed() const noexcept;

private:
    Player(const Script& script, const TrackSet& tracks);

    /// Gives events from next_ while `due(time)` holds for each; see
    /// give_due().
    template <typename Due>
    void give_while(const Due& due);

    /// Tries to give `event`. Returns false if its ring is full.
    [[nodiscard]] bool give(const Event& event);

    /// Reads the status block, keeping the last consistent copy.
    void read_status();

    const Script* script_;
    const TrackSet* tracks_;

    // Declared in this order so the engine, which refers to the state block
    // and the ring, is destroyed first.
    std::unique_ptr<ipc::EngineState> state_;
    std::unique_ptr<ipc::CommandRing> ring_;
    std::unique_ptr<engine::Engine> engine_;
    ipc::RingProducer<ipc::CommandSlot, ipc::kCommandRingCapacity> producer_;
    std::size_t client_ = 0;

    /// The next event to give.
    std::size_t next_ = 0;
    /// Events before this index have been counted as delayed, or given.
    std::size_t counted_ = 0;
    std::uint64_t sequence_ = 0;
    std::uint64_t delayed_ = 0;
    /// The last consistent copy of the status block; zero before the first.
    ipc::EngineStatus status_{};
};

} // namespace ks::harness
