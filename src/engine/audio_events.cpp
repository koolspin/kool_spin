// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/audio_events.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace ks::engine {

namespace {

// A full window's summary says "in the last second".
static_assert(kEventSummaryInterval == std::chrono::seconds{1});

/// A rejection's reason, as lines give it (spec requirement 7).
std::string_view reason(Rejection rejection) noexcept {
    switch (rejection) {
    case Rejection::UnknownType:
        return "an unknown type";
    case Rejection::BadTarget:
        return "a bad target";
    case Rejection::NonzeroPadding:
        return "nonzero padding";
    case Rejection::BadValue:
        return "a bad value";
    }
    return "an unknown reason";
}

/// `singular`, with an "s" unless `count` is 1.
std::string noun(std::uint64_t count, std::string_view singular) {
    std::string word{singular};
    if (count != 1) {
        word += 's';
    }
    return word;
}

double seconds(std::chrono::nanoseconds duration) noexcept {
    return std::chrono::duration<double>(duration).count();
}

double milliseconds(std::chrono::nanoseconds duration) noexcept {
    return std::chrono::duration<double, std::milli>(duration).count();
}

/// The line for the first event of a quiet stream.
EventLine first_line(const AudioEvent& event) {
    const std::uint64_t count = event.count;
    switch (event.kind) {
    case AudioEventKind::LateBlock:
        return EventLine{.level = ipc::LogLevel::Warning,
                         .text = std::format("late block: rendered in {:.3f} ms, load {:.2f}",
                                             milliseconds(event.duration), event.load)};
    case AudioEventKind::NonFinite:
        return EventLine{.level = ipc::LogLevel::Warning,
                         .text = std::format("silenced {} non-finite samples", count)};
    case AudioEventKind::Rejected:
        return EventLine{.level = ipc::LogLevel::Warning,
                         .text =
                             std::format("rejected {} {} from slot {}; the last: {}, sequence {}",
                                         count, noun(count, "command"), event.index,
                                         reason(event.rejection), event.sequence)};
    case AudioEventKind::CorruptRing:
        return EventLine{.level = ipc::LogLevel::Error,
                         .text =
                             std::format("the ring in slot {} is corrupt; its commands are ignored",
                                         event.index)};
    case AudioEventKind::Waiting:
        return EventLine{
            .level = ipc::LogLevel::Warning,
            .text = std::format("deck {} is waiting for decoding", std::uint64_t{event.index} + 1)};
    case AudioEventKind::WaitEnded:
        return EventLine{.level = ipc::LogLevel::Info,
                         .text =
                             std::format("deck {} stopped waiting after {:.3f} s",
                                         std::uint64_t{event.index} + 1, seconds(event.duration))};
    }
    return EventLine{.level = ipc::LogLevel::Warning, .text = "an unknown audio-thread event"};
}

} // namespace

std::vector<EventLine> EventLines::add(const AudioEvent& event, Clock::time_point now) {
    std::vector<EventLine> out;
    end_windows(now, out);
    Key key{.group = Group::LateBlocks, .index = 0};
    switch (event.kind) {
    case AudioEventKind::LateBlock:
        break;
    case AudioEventKind::NonFinite:
        key.group = Group::NonFinite;
        break;
    case AudioEventKind::Rejected:
        key = Key{.group = Group::Rejections, .index = event.index};
        break;
    case AudioEventKind::CorruptRing:
        key = Key{.group = Group::CorruptRings, .index = event.index};
        break;
    case AudioEventKind::Waiting:
        key = Key{.group = Group::Waiting, .index = event.index};
        break;
    case AudioEventKind::WaitEnded:
        key = Key{.group = Group::WaitsEnded, .index = event.index};
        break;
    }
    take(key, event, now, out);
    return out;
}

std::vector<EventLine> EventLines::dropped(std::uint64_t total, Clock::time_point now) {
    std::vector<EventLine> out;
    end_windows(now, out);
    if (total > dropped_total_) {
        const std::uint64_t more = total - dropped_total_;
        const auto found = windows_.find(Key{.group = Group::Dropped, .index = 0});
        if (found == windows_.end()) {
            out.push_back(EventLine{
                .level = ipc::LogLevel::Warning,
                .text =
                    std::format("dropped {} audio-thread events: the event ring was full", more)});
            windows_.emplace(Key{.group = Group::Dropped, .index = 0},
                             Window{.start = now, .end = now + kEventSummaryInterval});
        } else {
            found->second.count += more;
        }
    }
    // The count only goes up; if it ever didn't, the new value is the base.
    dropped_total_ = total;
    return out;
}

std::vector<EventLine> EventLines::tick(Clock::time_point now) {
    std::vector<EventLine> out;
    end_windows(now, out);
    return out;
}

std::vector<EventLine> EventLines::flush(Clock::time_point now) {
    std::vector<EventLine> out;
    end_windows(now, out);
    for (auto& [key, window] : windows_) {
        if (window.count == 0) {
            continue;
        }
        out.push_back(
            summary(key, window, std::format("in the last {:.3f} s", seconds(now - window.start))));
        // Counted once: a later tick or flush starts from nothing.
        window = Window{.start = now, .end = window.end};
    }
    return out;
}

void EventLines::take(Key key, const AudioEvent& event, Clock::time_point now,
                      std::vector<EventLine>& out) {
    const auto found = windows_.find(key);
    if (found == windows_.end()) {
        out.push_back(first_line(event));
        windows_.emplace(key, Window{.start = now, .end = now + kEventSummaryInterval});
        return;
    }
    Window& window = found->second;
    window.count += event.count;
    window.highest_load = std::max(window.highest_load, event.load);
    window.longest = std::max(window.longest, event.duration);
    window.sequence = event.sequence;
    window.rejection = event.rejection;
}

void EventLines::end_windows(Clock::time_point now, std::vector<EventLine>& out) {
    for (auto it = windows_.begin(); it != windows_.end();) {
        Window& window = it->second;
        // A window that counted something gets its summary, and the next
        // one starts where it ended; one that counted nothing ends the
        // stream's window.
        while (now >= window.end && window.count > 0) {
            out.push_back(summary(it->first, window, "in the last second"));
            window = Window{.start = window.end, .end = window.end + kEventSummaryInterval};
        }
        if (now >= window.end) {
            it = windows_.erase(it);
        } else {
            ++it;
        }
    }
}

EventLine EventLines::summary(Key key, const Window& window, std::string_view ending) {
    const std::uint64_t count = window.count;
    const std::uint64_t index = key.index;
    switch (key.group) {
    case Group::LateBlocks:
        return EventLine{.level = ipc::LogLevel::Warning,
                         .text = std::format("late blocks: {} more {}, highest load {:.2f}", count,
                                             ending, window.highest_load)};
    case Group::NonFinite:
        return EventLine{.level = ipc::LogLevel::Warning,
                         .text =
                             std::format("non-finite samples: {} more silenced {}", count, ending)};
    case Group::Rejections:
        return EventLine{
            .level = ipc::LogLevel::Warning,
            .text = std::format("rejected {} more {} from slot {} {}; the last: {}, sequence {}",
                                count, noun(count, "command"), index, ending,
                                reason(window.rejection), window.sequence)};
    case Group::CorruptRings:
        return EventLine{.level = ipc::LogLevel::Error,
                         .text = std::format("the ring in slot {} was found corrupt {} more {} {}",
                                             index, count, noun(count, "time"), ending)};
    case Group::Waiting:
        return EventLine{.level = ipc::LogLevel::Warning,
                         .text = std::format("deck {} waited for decoding {} more {} {}", index + 1,
                                             count, noun(count, "time"), ending)};
    case Group::WaitsEnded:
        return EventLine{
            .level = ipc::LogLevel::Info,
            .text = std::format("deck {} stopped waiting {} more {} {}, the longest after {:.3f} s",
                                index + 1, count, noun(count, "time"), ending,
                                seconds(window.longest))};
    case Group::Dropped:
        return EventLine{.level = ipc::LogLevel::Warning,
                         .text =
                             std::format("dropped {} more audio-thread events {}", count, ending)};
    }
    return EventLine{.level = ipc::LogLevel::Warning, .text = "an unknown audio-thread event"};
}

} // namespace ks::engine
