// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The render core: the engine's one render function, and the token that lets
/// only one driver call it at a time (ADR 0003 §1).

#include <ks/core/realtime.h>
#include <ks/engine/block.h>
#include <ks/engine/safety.h>

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ks::engine {

/// Where the output goes, as the output controller on the control thread
/// sees it (ADR 0003 §15).
enum class OutputState : std::uint8_t {
    Stopped,
    /// The device is the active driver.
    Playing,
    /// The device is lost; the freewheel driver keeps the clock running.
    Freewheeling,
    /// The device is back and started, but not yet active.
    Attaching,
};

/// The render core's values after a block it rendered, as its accessors
/// return them (specs/state-out/spec.md requirement 15).
struct RenderStats {
    std::uint64_t blocks_rendered = 0;
    std::uint64_t frames_rendered = 0;
    std::uint64_t non_finite_samples = 0;
    std::uint64_t limited_frames = 0;
    std::uint64_t late_blocks = 0;
    /// This block's render time.
    std::chrono::nanoseconds render_time{};
    /// This block's render load.
    double render_load = 0.0;
    double peak_render_load = 0.0;
    OutputState output_state = OutputState::Stopped;
    bool muted = false;
    bool silenced = false;

    bool operator==(const RenderStats&) const = default;
};

/// What the render core renders: the engine, or a test's graph.
class Graph {
public:
    /// Writes the block's channels. They hold silence when it's called.
    virtual void process(const Block& block) noexcept KS_NONBLOCKING = 0;

    /// Called by the render core once for each block it renders, after the
    /// safety stage and after updating every count, while it still holds the
    /// render token. `block` is the block the graph rendered, and `stats` the
    /// core's values after it. Does nothing by default.
    virtual void rendered(const Block& /*block*/,
                          const RenderStats& /*stats*/) noexcept KS_NONBLOCKING {}

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
/// the active driver with activate(). The output safety stage runs here,
/// after the graph, so no driver can bypass it.
class RenderCore {
public:
    /// The graph must outlive the core. Production code uses the default
    /// safety configuration.
    explicit RenderCore(Graph& graph, const SafetyConfig& safety = {}) noexcept;

    RenderCore(const RenderCore&) = delete;
    RenderCore& operator=(const RenderCore&) = delete;
    RenderCore(RenderCore&&) = delete;
    RenderCore& operator=(RenderCore&&) = delete;
    ~RenderCore() = default;

    /// Renders `block` if it's valid, `driver` is the active driver, and no
    /// other driver is rendering: takes the render token, checks the driver,
    /// fills the block's channels with silence, runs the graph (with
    /// Block::resumed set) and the safety stage, counts the frames, times the
    /// block, calls the graph's rendered(), and releases the token. Denormals
    /// are flushed while it renders.
    RenderResult try_render(const Block& block,
                            DriverId driver = kDefaultDriver) noexcept KS_NONBLOCKING;

    /// The total number of frames in rendered blocks. Any thread may call it.
    [[nodiscard]] std::uint64_t frames_rendered() const noexcept KS_NONBLOCKING;

    // Render timing (specs/state-out/spec.md requirements 12 and 13). A
    // block's render time runs from when its driver check passes to the end
    // of the safety stage, read with ks::core::monotonic_now(). Its render
    // load is the render time divided by the block's duration. Any thread may
    // call these.

    /// The number of rendered blocks: the heartbeat.
    [[nodiscard]] std::uint64_t blocks_rendered() const noexcept KS_NONBLOCKING;
    /// Rendered blocks whose render time was longer than their duration.
    [[nodiscard]] std::uint64_t late_blocks() const noexcept KS_NONBLOCKING;
    /// The last rendered block's render time; 0 before the first.
    [[nodiscard]] std::chrono::nanoseconds render_time() const noexcept KS_NONBLOCKING;
    /// The last rendered block's render load; 0 before the first.
    [[nodiscard]] double render_load() const noexcept KS_NONBLOCKING;
    /// The highest render load of any rendered block; 0 before the first.
    [[nodiscard]] double peak_render_load() const noexcept KS_NONBLOCKING;

    /// Makes `driver` the active driver. Any thread may call it.
    void activate(DriverId driver) noexcept KS_NONBLOCKING;

    /// The active driver. Any thread may call it.
    [[nodiscard]] DriverId active_driver() const noexcept KS_NONBLOCKING;

    /// The host time at which the last rendered block ended, or `time_point{}`
    /// before anything has rendered. Any thread may call it.
    [[nodiscard]] std::chrono::steady_clock::time_point
    rendered_until() const noexcept KS_NONBLOCKING;

    /// Mutes or unmutes the output, fading over the safety stage's fade time,
    /// from the start of the next rendered block. Any thread may call it.
    void set_muted(bool muted) noexcept KS_NONBLOCKING;

    /// Whether set_muted() last muted the output. Any thread may call it.
    [[nodiscard]] bool muted() const noexcept KS_NONBLOCKING;

    /// True when muted and the fade has reached silence by the end of the
    /// last rendered block. Any thread may call it.
    [[nodiscard]] bool silenced() const noexcept KS_NONBLOCKING;

    /// NaN and infinite samples the safety stage has silenced. Any thread
    /// may call it.
    [[nodiscard]] std::uint64_t non_finite_samples() const noexcept KS_NONBLOCKING;

    /// Frames in which the safety stage's limiter reduced the output. Any
    /// thread may call it.
    [[nodiscard]] std::uint64_t limited_frames() const noexcept KS_NONBLOCKING;

    /// The safety stage's configuration. Any thread may call it.
    [[nodiscard]] const SafetyConfig& safety_config() const noexcept KS_NONBLOCKING;

    /// Stores where the output goes, for the status the graph publishes. The
    /// output controller calls it whenever its state changes. Any thread may
    /// call it (specs/state-out/spec.md requirement 14).
    void set_output_state(OutputState state) noexcept KS_NONBLOCKING;

    /// What set_output_state() last stored; Stopped until then. Any thread
    /// may call it.
    [[nodiscard]] OutputState output_state() const noexcept KS_NONBLOCKING;

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

    /// Only the token's holder uses it.
    SafetyStage safety_;
    std::atomic<bool> muted_{false};
    /// Published from safety_ after each rendered block.
    std::atomic<bool> silenced_{false};
    std::atomic<std::uint64_t> non_finite_samples_{0};
    std::atomic<std::uint64_t> limited_frames_{0};

    // Render timing. Only the token's holder writes these.
    std::atomic<std::uint64_t> blocks_rendered_{0};
    std::atomic<std::uint64_t> late_blocks_{0};
    /// render_time(), in nanoseconds.
    std::atomic<std::int64_t> render_time_ns_{0};
    /// render_load() and peak_render_load(), as a double's bit pattern, so
    /// they're lock-free on every platform.
    std::atomic<std::uint64_t> render_load_bits_{0};
    std::atomic<std::uint64_t> peak_render_load_bits_{0};

    std::atomic<OutputState> output_state_{OutputState::Stopped};
};

} // namespace ks::engine
