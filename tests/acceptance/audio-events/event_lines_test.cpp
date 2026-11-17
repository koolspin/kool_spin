// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/audio-events/spec.md requirements 5 to 7 and 10:
// EventLines, which turns the audio thread's events into log lines, the first
// of each stream at once and then a summary at most once a second. The tests
// give it the time, so nothing here waits.

#include <ks/engine/audio_events.h>
#include <ks/engine/command.h>
#include <ks/ipc/log.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ks::engine::AudioEvent;
using ks::engine::AudioEventKind;
using ks::engine::EventLine;
using ks::engine::EventLines;
using ks::engine::Rejection;
using ks::ipc::LogLevel;

using Clock = EventLines::Clock;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;

/// The tests' time 0: an arbitrary time, well clear of the clock's epoch.
constexpr Clock::time_point kZero{std::chrono::hours{1}};

/// `ms` milliseconds after kZero.
constexpr Clock::time_point at(std::int64_t ms) {
    return kZero + milliseconds{ms};
}

AudioEvent late(double load, nanoseconds render_time = milliseconds{12}) {
    return AudioEvent{.kind = AudioEventKind::LateBlock,
                      .count = 1,
                      .load = load,
                      .duration = render_time,
                      .host_time = kZero};
}

AudioEvent non_finite(std::uint64_t samples) {
    return AudioEvent{.kind = AudioEventKind::NonFinite, .count = samples, .host_time = kZero};
}

AudioEvent rejected(std::uint32_t slot, std::uint64_t count, std::uint64_t sequence,
                    Rejection reason) {
    return AudioEvent{.kind = AudioEventKind::Rejected,
                      .index = slot,
                      .count = count,
                      .sequence = sequence,
                      .rejection = reason,
                      .host_time = kZero};
}

AudioEvent corrupt(std::uint32_t slot) {
    return AudioEvent{
        .kind = AudioEventKind::CorruptRing, .index = slot, .count = 1, .host_time = kZero};
}

AudioEvent waiting(std::uint32_t deck) {
    return AudioEvent{
        .kind = AudioEventKind::Waiting, .index = deck, .count = 1, .host_time = kZero};
}

AudioEvent wait_ended(std::uint32_t deck, nanoseconds waited) {
    return AudioEvent{.kind = AudioEventKind::WaitEnded,
                      .index = deck,
                      .count = 1,
                      .duration = waited,
                      .host_time = kZero};
}

bool contains(const EventLine& line, std::string_view phrase) {
    return line.text.find(phrase) != std::string::npos;
}

} // namespace

TEST_CASE("Event lines: the first at once, then once a second", "[audio-events]") {
    EventLines lines;
    const std::vector<EventLine> first = lines.add(late(1.15, nanoseconds{12'345'000}), at(0));
    REQUIRE(first.size() == 1);
    CAPTURE(first.front().text);
    CHECK(first.front().level == LogLevel::Warning);
    CHECK(contains(first.front(), "late block"));
    CHECK(contains(first.front(), "load 1.15"));

    // 36 more within the window, the highest load 1.83: counted, not written.
    for (int i = 1; i <= 36; ++i) {
        const double load = i == 20 ? 1.83 : 1.0 + (i * 0.01);
        CAPTURE(i);
        CHECK(lines.add(late(load), at(std::int64_t{i} * 25)).empty());
    }
    CHECK(lines.tick(at(990)).empty());

    const std::vector<EventLine> summary = lines.tick(at(1'000));
    REQUIRE(summary.size() == 1);
    CAPTURE(summary.front().text);
    CHECK(summary.front().level == LogLevel::Warning);
    CHECK(contains(summary.front(), "late blocks: 36 more in the last second"));
    CHECK(contains(summary.front(), "highest load 1.83"));

    // The next window counts nothing, so the stream goes quiet, and its next
    // event is written at once.
    CHECK(lines.tick(at(2'000)).empty());
    const std::vector<EventLine> again = lines.add(late(1.2), at(2'500));
    REQUIRE(again.size() == 1);
    CAPTURE(again.front().text);
    CHECK(contains(again.front(), "late block"));
    CHECK_FALSE(contains(again.front(), "more"));
    CHECK(lines.tick(at(3'500)).empty());
}

TEST_CASE("Event lines: each stream on its own", "[audio-events]") {
    EventLines lines;
    const std::vector<EventLine> three = lines.add(rejected(3, 1, 7, Rejection::BadTarget), at(0));
    REQUIRE(three.size() == 1);
    CHECK(contains(three.front(), "rejected 1 command from slot 3"));
    const std::vector<EventLine> zero = lines.add(rejected(0, 1, 1, Rejection::UnknownType), at(0));
    REQUIRE(zero.size() == 1);
    CHECK(contains(zero.front(), "rejected 1 command from slot 0"));

    CHECK(lines.add(rejected(3, 4, 11, Rejection::NonzeroPadding), at(200)).empty());
    CHECK(lines.add(rejected(0, 2, 3, Rejection::BadValue), at(300)).empty());
    // Another stream: written at once, at its own level.
    const std::vector<EventLine> broken = lines.add(corrupt(0), at(400));
    REQUIRE(broken.size() == 1);
    CHECK(broken.front().level == LogLevel::Error);
    CHECK(contains(broken.front(), "the ring in slot 0 is corrupt"));
    CHECK(lines.add(rejected(0, 1, 4, Rejection::UnknownType), at(500)).empty());

    // Both windows end at 1 s: slot 0's summary comes first.
    const std::vector<EventLine> summaries = lines.tick(at(1'000));
    REQUIRE(summaries.size() == 2);
    CAPTURE(summaries.at(0).text, summaries.at(1).text);
    CHECK(summaries.at(0).level == LogLevel::Warning);
    CHECK(contains(summaries.at(0), "rejected 3 more commands from slot 0 in the last second"));
    CHECK(contains(summaries.at(0), "an unknown type, sequence 4"));
    CHECK(summaries.at(1).level == LogLevel::Warning);
    CHECK(contains(summaries.at(1), "rejected 4 more commands from slot 3 in the last second"));
    CHECK(contains(summaries.at(1), "nonzero padding, sequence 11"));

    // The corrupt ring's window counted nothing more.
    CHECK(lines.tick(at(1'400)).empty());
}

TEST_CASE("Event lines: levels and wording", "[audio-events]") {
    struct Case {
        AudioEvent event;
        LogLevel level;
        std::vector<std::string_view> phrases;
    };
    const std::vector<Case> cases{
        {.event = late(1.15, nanoseconds{12'345'000}),
         .level = LogLevel::Warning,
         .phrases = {"late block", "rendered in 12.345 ms", "load 1.15"}},
        {.event = non_finite(12),
         .level = LogLevel::Warning,
         .phrases = {"non-finite samples", "silenced 12 non-finite samples"}},
        {.event = rejected(2, 1, 1'234, Rejection::UnknownType),
         .level = LogLevel::Warning,
         .phrases = {"rejected 1 command from slot 2", "an unknown type", "sequence 1234"}},
        {.event = rejected(2, 2, 1'235, Rejection::BadTarget),
         .level = LogLevel::Warning,
         .phrases = {"rejected 2 commands from slot 2", "a bad target", "sequence 1235"}},
        {.event = rejected(2, 1, 1, Rejection::NonzeroPadding),
         .level = LogLevel::Warning,
         .phrases = {"nonzero padding"}},
        {.event = rejected(2, 1, 1, Rejection::BadValue),
         .level = LogLevel::Warning,
         .phrases = {"a bad value"}},
        {.event = corrupt(5),
         .level = LogLevel::Error,
         .phrases = {"the ring in slot 5 is corrupt"}},
        {.event = waiting(0),
         .level = LogLevel::Warning,
         .phrases = {"deck 1 is waiting for decoding"}},
        {.event = wait_ended(3, milliseconds{420}),
         .level = LogLevel::Info,
         .phrases = {"deck 4 stopped waiting", "after 0.420 s"}},
    };
    for (const Case& c : cases) {
        EventLines lines;
        const std::vector<EventLine> out = lines.add(c.event, at(0));
        REQUIRE(out.size() == 1);
        CAPTURE(out.front().text);
        CHECK(out.front().level == c.level);
        for (const std::string_view phrase : c.phrases) {
            CAPTURE(phrase);
            CHECK(contains(out.front(), phrase));
        }
    }
}

TEST_CASE("Event lines: dropped events and flushing", "[audio-events]") {
    EventLines lines;
    CHECK(lines.dropped(0, at(0)).empty());
    const std::vector<EventLine> first = lines.dropped(76, at(100));
    REQUIRE(first.size() == 1);
    CAPTURE(first.front().text);
    CHECK(first.front().level == LogLevel::Warning);
    CHECK(contains(first.front(), "dropped 76 audio-thread events"));

    CHECK(lines.dropped(100, at(500)).empty());
    const std::vector<EventLine> flushed = lines.flush(at(600));
    REQUIRE(flushed.size() == 1);
    CAPTURE(flushed.front().text);
    CHECK(flushed.front().level == LogLevel::Warning);
    CHECK(contains(flushed.front(), "dropped 24 more audio-thread events in the last 0.500 s"));
}
