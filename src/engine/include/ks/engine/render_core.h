// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The render core: the engine's one render function, and the token that lets
/// only one driver call it at a time (ADR 0003 §1).

#include <ks/core/realtime.h>
#include <ks/engine/block.h>

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ks::engine {

/// What the render core renders. Later steps make the decks and the mixer a
/// graph; for now, only tests implement one.
class Graph {
public:
    /// Writes the block's channels. They hold silence when it's called.
    virtual void process(const Block& block) noexcept KS_NONBLOCKING = 0;

protected:
    Graph() = default;
    Graph(const Graph&) = default;
    Graph& operator=(const Graph&) = default;
    Graph(Graph&&) = default;
    Graph& operator=(Graph&&) = default;
    ~Graph() = default;
};

/// Identifies a driver to the render core (ADR 0003 §1).
enum class DriverId : std::uint32_t {};

/// The driver that's active until another is chosen.
inline constexpr DriverId kDefaultDriver{0};

/// What try_render() did with a block.
enum class RenderResult : std::uint8_t {
    /// The graph rendered it.
    Rendered,
    /// The block's driver isn't the active one, or another driver holds the
    /// render token. The block's channels are silent.
    Skipped,
    /// The block isn't valid (see Block). Its channels are untouched.
    Rejected,
};

/// Renders blocks through a graph, for the active driver, one block at a time.
///
/// Every driver calls try_render() for each block. The control thread chooses
/// the active driver with activate(). The output safety stage (step 3) will
/// run here, after the graph, so no driver can bypass it.
class RenderCore {
public:
    /// The graph must outlive the core.
    explicit RenderCore(Graph& graph) noexcept;

    RenderCore(const RenderCore&) = delete;
    RenderCore& operator=(const RenderCore&) = delete;
    RenderCore(RenderCore&&) = delete;
    RenderCore& operator=(RenderCore&&) = delete;
    ~RenderCore() = default;

    /// Renders `block` if it's valid, `driver` is the active driver, and no
    /// other driver is rendering: takes the render token, checks the driver,
    /// fills the block's channels with silence, runs the graph (with
    /// Block::resumed set), counts the frames, and releases the token.
    RenderResult try_render(const Block& block,
                            DriverId driver = kDefaultDriver) noexcept KS_NONBLOCKING;

    /// The total number of frames in rendered blocks. Any thread may call it.
    [[nodiscard]] std::uint64_t frames_rendered() const noexcept KS_NONBLOCKING;

    /// Makes `driver` the active driver. Any thread may call it.
    void activate(DriverId driver) noexcept KS_NONBLOCKING;

    /// The active driver. Any thread may call it.
    [[nodiscard]] DriverId active_driver() const noexcept KS_NONBLOCKING;

    /// The host time at which the last rendered block ended, or `time_point{}`
    /// before anything has rendered. Any thread may call it.
    [[nodiscard]] std::chrono::steady_clock::time_point
    rendered_until() const noexcept KS_NONBLOCKING;

private:
    Graph* graph_;
    /// True while a driver is rendering. Taken with acquire and released
    /// with release, so each render sees the state the last one left.
    std::atomic<bool> token_{false};
    std::atomic<DriverId> active_{kDefaultDriver};
    std::atomic<std::uint64_t> frames_rendered_{0};
    /// rendered_until(), as nanoseconds since the steady clock's epoch.
    std::atomic<std::int64_t> rendered_until_ns_{0};
    /// The driver of the last rendered block. Only the token's holder uses it.
    DriverId last_driver_ = kDefaultDriver;
    /// Whether anything has rendered yet. Only the token's holder uses it.
    bool rendered_any_ = false;
};

} // namespace ks::engine
