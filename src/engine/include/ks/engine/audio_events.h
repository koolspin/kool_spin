// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The audio thread's events (specs/audio-events/spec.md; ADR 0003 §13):
/// what the audio thread records about each block, in a private lock-free
/// ring, and the control thread's EventLines, which turns those records into
/// log lines, summarized once a second.

#include <ks/engine/command.h>
#include <ks/ipc/log.h>

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ks::engine {

/// What happened (spec requirements 1 and 2).
enum class AudioEventKind : std::uint8_t {
    /// Rendering took longer than the block lasts.
    LateBlock,
    /// The safety stage silenced NaN or infinite samples.
    NonFinite,
    /// A client slot's ring had slots that decode() rejected.
    Rejected,
    /// A client slot's ring was found corrupt; it isn't read any more.
    CorruptRing,
    /// A deck started waiting for decoding.
    Waiting,
    /// A deck stopped waiting, for whatever reason.
    WaitEnded,
};

/// One thing that happened in one block (spec requirement 1). Fields a kind
/// doesn't use are zero, and `rejection` is UnknownType.
struct AudioEvent {
    AudioEventKind kind = AudioEventKind::LateBlock;
    /// The client slot (Rejected, CorruptRing) or the deck (Waiting,
    /// WaitEnded), from 0.
    std::uint32_t index = 0;
    /// Late blocks, samples silenced, or commands rejected; 1 for the others.
    std::uint64_t count = 0;
    /// Rejected: the sequence number of the last slot rejected.
    std::uint64_t sequence = 0;
    /// Rejected: the last slot's reason.
    Rejection rejection = Rejection::UnknownType;
    /// LateBlock: the block's render load.
    double load = 0.0;
    /// LateBlock: the block's render time. WaitEnded: how long the deck
    /// waited, from the host time of the block in which it started waiting
    /// to this block's.
    std::chrono::nanoseconds duration{};
    /// The block's host time.
    std::chrono::steady_clock::time_point host_time;

    bool operator==(const AudioEvent&) const = default;
};

static_assert(std::is_trivially_copyable_v<AudioEvent>, "events are copied through a ring");

/// The event ring's capacity (spec requirement 3).
inline constexpr std::size_t kAudioEventCapacity = 1'024;

/// How long a summary window lasts (spec requirement 6).
inline constexpr std::chrono::seconds kEventSummaryInterval{1};

/// One line for the log.
struct EventLine {
    ipc::LogLevel level = ipc::LogLevel::Info;
    std::string text;

    bool operator==(const EventLine&) const = default;
};

/// Turns the audio thread's events into log lines, on the control thread
/// (spec requirements 5 to 7). The first event of a stream becomes a line at
/// once; while events keep coming, the stream gets one summary line a
/// second.
///
/// Each call takes the time it's made, `now`, on the steady clock, and
/// returns the lines to write now, in order. `now` never goes back.
class EventLines {
public:
    using Clock = std::chrono::steady_clock;

    /// One event, taken from the engine at `now`. First does what tick(now)
    /// would.
    [[nodiscard]] std::vector<EventLine> add(const AudioEvent& event, Clock::time_point now);

    /// The engine's dropped_events() total, read at `now`. An increase since
    /// the last call counts as one event on the dropped-events stream, with
    /// that count. First does what tick(now) would.
    [[nodiscard]] std::vector<EventLine> dropped(std::uint64_t total, Clock::time_point now);

    /// A summary for every stream whose window ended by `now` and counted
    /// anything.
    [[nodiscard]] std::vector<EventLine> tick(Clock::time_point now);

    /// As tick(), and also a summary for every open window that counted
    /// anything, however short: for when the engine stops.
    [[nodiscard]] std::vector<EventLine> flush(Clock::time_point now);

private:
    /// The streams, in the order their summaries come (spec requirement 6).
    enum class Group : std::uint8_t {
        LateBlocks,
        NonFinite,
        Rejections,
        CorruptRings,
        Waiting,
        WaitsEnded,
        Dropped,
    };

    /// A stream: a group, and the slot or deck for the groups that have one.
    struct Key {
        Group group = Group::LateBlocks;
        std::uint32_t index = 0;

        auto operator<=>(const Key&) const = default;
    };

    /// A stream's open window, if it has one.
    struct Window {
        Clock::time_point start;
        Clock::time_point end;
        /// The events' counts, added up.
        std::uint64_t count = 0;
        /// The highest load, for late blocks.
        double highest_load = 0.0;
        /// The longest wait, for ended waits.
        std::chrono::nanoseconds longest{};
        /// The last rejection, for rejections.
        std::uint64_t sequence = 0;
        Rejection rejection = Rejection::UnknownType;
    };

    /// Takes `event`, on its stream `key`: a first line in `out` if the
    /// stream is quiet, and otherwise counted in its window.
    void take(Key key, const AudioEvent& event, Clock::time_point now, std::vector<EventLine>& out);
    /// Ends every window that has ended by `now`, appending their summaries
    /// to `out`.
    void end_windows(Clock::time_point now, std::vector<EventLine>& out);
    /// The summary of `window`, on stream `key`, with `ending` saying how
    /// long the window was.
    [[nodiscard]] static EventLine summary(Key key, const Window& window, std::string_view ending);

    /// The open windows, by stream, so they're visited in summary order.
    std::map<Key, Window> windows_;
    /// The dropped-events total in the last call to dropped().
    std::uint64_t dropped_total_ = 0;
};

} // namespace ks::engine
