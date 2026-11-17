// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/audio-events/spec.md requirements 1 to 4: the
// events the engine's audio thread records, and the ring they pass through.
// The engine runs on the test's thread, as the offline driver runs it, so
// under the rtsan preset RealtimeSanitizer checks everything it records.
//
// Most tests call process() and rendered() as the render core does, with
// stats that report no late blocks, so how busy the machine is can't add
// events. "Audio events: non-finite samples" uses a real render core, for
// its safety stage.

#include <ks/engine/audio_events.h>
#include <ks/engine/block.h>
#include <ks/engine/command.h>
#include <ks/engine/engine.h>
#include <ks/engine/offline.h>
#include <ks/engine/render_core.h>
#include <ks/engine/track.h>
#include <ks/ipc/command.h>
#include <ks/ipc/layout.h>
#include <ks/ipc/state.h>

#include "../commands/test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace {

using ks::engine::AudioEvent;
using ks::engine::AudioEventKind;
using ks::engine::Block;
using ks::engine::Engine;
using ks::engine::frames_to_duration;
using ks::engine::kAudioEventCapacity;
using ks::engine::kMaxBlockFrames;
using ks::engine::Rejection;
using ks::engine::RenderCore;
using ks::engine::RenderStats;
using ks::engine::test::Client;
using ks::engine::test::deck_command;
using ks::engine::test::encode;
using ks::engine::test::OwnedTrack;
using ks::engine::test::playing;
using ks::ipc::CommandSlot;
using ks::ipc::CommandType;

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;

constexpr std::uint32_t kRate = 48'000;
constexpr std::size_t kChannels = 4;
/// The first block's host time: an arbitrary time, well clear of the
/// clock's epoch.
constexpr Clock::time_point kStart{std::chrono::hours{1}};

/// Every event the engine has recorded and the test hasn't taken yet, oldest
/// first.
std::vector<AudioEvent> take_all(Engine& engine) {
    std::vector<AudioEvent> events;
    while (const std::optional<AudioEvent> event = engine.take_event()) {
        events.push_back(*event);
    }
    return events;
}

/// Output buffers for one block of up to kMaxBlockFrames frames.
class Outputs {
public:
    Outputs() {
        for (std::size_t c = 0; c < kChannels; ++c) {
            storage_.at(c).assign(kMaxBlockFrames, 0.0F);
            views_.at(c) = std::span{storage_.at(c)};
        }
    }

    Outputs(const Outputs&) = delete;
    Outputs& operator=(const Outputs&) = delete;
    Outputs(Outputs&&) = delete;
    Outputs& operator=(Outputs&&) = delete;
    ~Outputs() = default;

    /// A block of `frames` frames at `host_time`, its channels silent, as
    /// the render core passes it to the graph.
    [[nodiscard]] Block block(std::size_t frames, Clock::time_point host_time) {
        for (std::vector<float>& channel : storage_) {
            std::ranges::fill(channel, 0.0F);
        }
        return Block{.channels = views_,
                     .frames = frames,
                     .sample_rate = kRate,
                     .host_time = host_time,
                     .output_latency = {},
                     .resumed = false};
    }

private:
    std::array<std::vector<float>, kChannels> storage_;
    std::array<std::span<float>, kChannels> views_{};
};

/// An engine without a state block, driven as the render core drives it:
/// process(), then rendered() with stats that report nothing late or
/// silenced, at consecutive host times from kStart.
class Rig {
public:
    Rig() : engine_{std::make_unique<Engine>()} {}

    /// Renders a block of `frames` frames at the next host time, and returns
    /// that time.
    Clock::time_point render(std::size_t frames) {
        const Clock::time_point at = kStart + frames_to_duration(frames_, kRate);
        const Block block = outputs_.block(frames, at);
        engine_->process(block);
        ++blocks_;
        frames_ += frames;
        engine_->rendered(block, RenderStats{.blocks_rendered = blocks_,
                                             .frames_rendered = frames_,
                                             .non_finite_samples = 0,
                                             .limited_frames = 0,
                                             .late_blocks = 0,
                                             .render_time = nanoseconds{100'000},
                                             .render_load = 0.01,
                                             .peak_render_load = 0.01});
        return at;
    }

    [[nodiscard]] Engine& engine() { return *engine_; }

private:
    std::unique_ptr<Engine> engine_;
    Outputs outputs_;
    std::uint64_t blocks_ = 0;
    std::uint64_t frames_ = 0;
};

/// A constant 48 kHz track of `frames` frames.
std::unique_ptr<OwnedTrack> steady_track(std::uint64_t frames) {
    return std::make_unique<OwnedTrack>(
        frames, kRate, [](std::uint64_t /*frame*/, std::size_t /*channel*/) { return 0.25F; });
}

} // namespace

TEST_CASE("Audio events: late blocks", "[audio-events]") {
    struct Call {
        std::uint64_t late_blocks;
        double load;
        nanoseconds render_time;
    };
    constexpr std::array<Call, 4> kCalls{
        Call{.late_blocks = 0, .load = 0.5, .render_time = nanoseconds{5'000'000}},
        Call{.late_blocks = 1, .load = 1.25, .render_time = nanoseconds{12'500'000}},
        Call{.late_blocks = 1, .load = 0.6, .render_time = nanoseconds{6'000'000}},
        Call{.late_blocks = 3, .load = 1.75, .render_time = nanoseconds{17'500'000}}};
    constexpr std::size_t kFrames = 480;

    for (const bool with_state : {false, true}) {
        CAPTURE(with_state);
        auto state = std::make_unique<ks::ipc::EngineState>();
        auto engine = with_state ? std::make_unique<Engine>(*state) : std::make_unique<Engine>();
        Outputs outputs;
        std::vector<Clock::time_point> times;
        times.reserve(kCalls.size());
        std::uint64_t blocks = 0;
        for (const Call& call : kCalls) {
            const Clock::time_point at = kStart + frames_to_duration(blocks * kFrames, kRate);
            ++blocks;
            engine->rendered(outputs.block(kFrames, at),
                             RenderStats{.blocks_rendered = blocks,
                                         .frames_rendered = blocks * kFrames,
                                         .non_finite_samples = 0,
                                         .limited_frames = 0,
                                         .late_blocks = call.late_blocks,
                                         .render_time = call.render_time,
                                         .render_load = call.load,
                                         .peak_render_load = 1.75});
            times.push_back(at);
        }

        const std::vector<AudioEvent> expected{AudioEvent{.kind = AudioEventKind::LateBlock,
                                                          .count = 1,
                                                          .load = 1.25,
                                                          .duration = nanoseconds{12'500'000},
                                                          .host_time = times.at(1)},
                                               AudioEvent{.kind = AudioEventKind::LateBlock,
                                                          .count = 2,
                                                          .load = 1.75,
                                                          .duration = nanoseconds{17'500'000},
                                                          .host_time = times.at(3)}};
        CHECK(take_all(*engine) == expected);
        CHECK(engine->dropped_events() == 0);
    }
}

TEST_CASE("Audio events: non-finite samples", "[audio-events]") {
    constexpr std::size_t kBlockFrames = 512;
    constexpr std::size_t kBlocks = 20;
    for (const bool with_nan : {true, false}) {
        CAPTURE(with_nan);
        auto engine = std::make_unique<Engine>();
        RenderCore core{*engine};
        const auto track = std::make_unique<OwnedTrack>(
            48'000, kRate, [with_nan](std::uint64_t frame, std::size_t /*channel*/) {
                return with_nan && frame % 100 == 0 ? std::numeric_limits<float>::quiet_NaN()
                                                    : 0.25F;
            });
        Client client;
        REQUIRE(engine->attach(client.ring()) == std::optional<std::size_t>{0});
        REQUIRE(engine->load(0, track->track()));
        REQUIRE(client.send(playing(0, true)));

        std::vector<std::vector<float>> output(kChannels,
                                               std::vector<float>(kBlocks * kBlockFrames, 0.0F));
        std::vector<std::span<float>> views;
        views.reserve(output.size());
        for (std::vector<float>& channel : output) {
            views.emplace_back(channel);
        }
        const ks::engine::OfflineConfig config{.sample_rate = kRate,
                                               .block_frames = kBlockFrames,
                                               .start_time = kStart,
                                               .driver = ks::engine::kDefaultDriver};
        REQUIRE(ks::engine::render_offline(core, config, views) == kBlocks * kBlockFrames);

        std::vector<Clock::time_point> block_times;
        block_times.reserve(kBlocks);
        for (std::size_t block = 0; block < kBlocks; ++block) {
            block_times.push_back(kStart + frames_to_duration(block * kBlockFrames, kRate));
        }
        // Late blocks depend on how busy the machine is, so only NonFinite
        // events are counted here.
        std::uint64_t silenced = 0;
        for (const AudioEvent& event : take_all(*engine)) {
            if (event.kind != AudioEventKind::NonFinite) {
                continue;
            }
            CAPTURE(event.count, event.host_time.time_since_epoch().count());
            CHECK(event.index == 0);
            CHECK(event.count > 0);
            CHECK(std::ranges::find(block_times, event.host_time) != block_times.end());
            silenced += event.count;
        }
        if (with_nan) {
            CHECK(core.non_finite_samples() > 0);
        } else {
            CHECK(core.non_finite_samples() == 0);
        }
        CHECK(silenced == core.non_finite_samples());
    }
}

TEST_CASE("Audio events: rejected commands", "[audio-events]") {
    Rig rig;
    std::array<Client, 3> clients;
    for (std::size_t slot = 0; slot < clients.size(); ++slot) {
        REQUIRE(rig.engine().attach(clients.at(slot).ring()) == std::optional<std::size_t>{slot});
    }
    static_cast<void>(rig.render(64));
    REQUIRE(take_all(rig.engine()).empty());

    // Valid, valid, an unknown type, valid, a bad value.
    Client& client = clients.at(2);
    CommandSlot unknown = encode(3, deck_command(CommandType::SetFader, 0, 0.3));
    unknown.type = 99;
    CommandSlot bad_value = encode(5, playing(0, true));
    bad_value.payload[0] = 2;
    REQUIRE(client.send_slot(encode(1, deck_command(CommandType::SetFader, 0, 0.5))));
    REQUIRE(client.send_slot(encode(2, deck_command(CommandType::SetFader, 1, 0.5))));
    REQUIRE(client.send_slot(unknown));
    REQUIRE(client.send_slot(encode(4, deck_command(CommandType::SetFader, 2, 0.5))));
    REQUIRE(client.send_slot(bad_value));
    const Clock::time_point at = rig.render(64);
    CHECK(take_all(rig.engine()) ==
          std::vector<AudioEvent>{AudioEvent{.kind = AudioEventKind::Rejected,
                                             .index = 2,
                                             .count = 2,
                                             .sequence = 5,
                                             .rejection = Rejection::BadValue,
                                             .host_time = at}});

    static_cast<void>(rig.render(64));
    CHECK(take_all(rig.engine()).empty());
}

TEST_CASE("Audio events: a corrupt ring", "[audio-events]") {
    Rig rig;
    Client first;
    Client second;
    REQUIRE(rig.engine().attach(first.ring()) == std::optional<std::size_t>{0});
    REQUIRE(rig.engine().attach(second.ring()) == std::optional<std::size_t>{1});
    static_cast<void>(rig.render(64));
    REQUIRE(take_all(rig.engine()).empty());

    // The head 2,000 past the tail: more than the ring holds.
    const std::uint64_t tail = ks::ipc::load_shared(second.ring().tail, std::memory_order_acquire);
    ks::ipc::store_shared(second.ring().head, tail + 2'000, std::memory_order_release);
    const Clock::time_point at = rig.render(64);
    CHECK(take_all(rig.engine()) ==
          std::vector<AudioEvent>{AudioEvent{
              .kind = AudioEventKind::CorruptRing, .index = 1, .count = 1, .host_time = at}});
    for (int block = 0; block < 10; ++block) {
        static_cast<void>(rig.render(64));
    }
    CHECK(take_all(rig.engine()).empty());

    // Detached, and its release collected, the slot is free again.
    REQUIRE(rig.engine().detach(1));
    bool released = false;
    for (int block = 0; block < 10 && !released; ++block) {
        static_cast<void>(rig.render(64));
        while (const std::optional<ks::engine::Released> release = rig.engine().collect()) {
            released = released || release->ring == &second.ring();
        }
    }
    REQUIRE(released);
    CHECK(take_all(rig.engine()).empty());

    // Another ring, corrupt before it's attached.
    Client third;
    ks::ipc::store_shared(third.ring().head, 2'000, std::memory_order_release);
    REQUIRE(rig.engine().attach(third.ring()) == std::optional<std::size_t>{1});
    const Clock::time_point again = rig.render(64);
    CHECK(take_all(rig.engine()) ==
          std::vector<AudioEvent>{AudioEvent{
              .kind = AudioEventKind::CorruptRing, .index = 1, .count = 1, .host_time = again}});
}

TEST_CASE("Audio events: a deck waiting for decoding", "[audio-events]") {
    constexpr std::size_t kBlockFrames = 480;
    Rig rig;
    const std::unique_ptr<OwnedTrack> track = steady_track(48'000);
    track->track().publish(9'600, false);
    Client client;
    REQUIRE(rig.engine().attach(client.ring()) == std::optional<std::size_t>{0});
    REQUIRE(rig.engine().load(1, track->track()));
    REQUIRE(client.send(playing(1, true)));
    const ks::engine::Deck& deck = rig.engine().mixer().deck(1);

    // It plays up to the limit of what's decoded, then waits.
    std::optional<Clock::time_point> started;
    for (int block = 0; block < 40 && !started; ++block) {
        CAPTURE(block);
        const bool was_waiting = deck.waiting();
        const Clock::time_point at = rig.render(kBlockFrames);
        const std::vector<AudioEvent> events = take_all(rig.engine());
        if (!deck.waiting()) {
            CHECK(events.empty());
            continue;
        }
        REQUIRE_FALSE(was_waiting);
        CHECK(events ==
              std::vector<AudioEvent>{AudioEvent{
                  .kind = AudioEventKind::Waiting, .index = 1, .count = 1, .host_time = at}});
        started = at;
    }
    REQUIRE(started.has_value());
    for (int block = 0; block < 5; ++block) {
        static_cast<void>(rig.render(kBlockFrames));
        REQUIRE(deck.waiting());
    }
    CHECK(take_all(rig.engine()).empty());

    // Decoding finishes: the next block plays on.
    track->track().publish(48'000, true);
    const Clock::time_point ended = rig.render(kBlockFrames);
    REQUIRE_FALSE(deck.waiting());
    CHECK(take_all(rig.engine()) ==
          std::vector<AudioEvent>{AudioEvent{.kind = AudioEventKind::WaitEnded,
                                             .index = 1,
                                             .count = 1,
                                             .duration = std::chrono::duration_cast<nanoseconds>(
                                                 ended - started.value_or(Clock::time_point{})),
                                             .host_time = ended}});
    for (int block = 0; block < 5; ++block) {
        static_cast<void>(rig.render(kBlockFrames));
    }
    CHECK(take_all(rig.engine()).empty());
}

TEST_CASE("Audio events: a full ring drops and counts", "[audio-events]") {
    constexpr std::size_t kCalls = 1'100;
    constexpr std::size_t kFrames = 480;
    auto engine = std::make_unique<Engine>();
    Outputs outputs;
    std::uint64_t blocks = 0;
    /// One more late block, at the next host time. Returns that time.
    const auto late_block = [&] {
        const Clock::time_point at = kStart + frames_to_duration(blocks * kFrames, kRate);
        ++blocks;
        engine->rendered(outputs.block(kFrames, at),
                         RenderStats{.blocks_rendered = blocks,
                                     .frames_rendered = blocks * kFrames,
                                     .non_finite_samples = 0,
                                     .limited_frames = 0,
                                     .late_blocks = blocks,
                                     .render_time = nanoseconds{11'000'000},
                                     .render_load = 1.1,
                                     .peak_render_load = 1.1});
        return at;
    };

    std::vector<Clock::time_point> times;
    times.reserve(kCalls);
    for (std::size_t call = 0; call < kCalls; ++call) {
        times.push_back(late_block());
    }
    CHECK(engine->dropped_events() == kCalls - kAudioEventCapacity);
    const std::vector<AudioEvent> events = take_all(*engine);
    REQUIRE(events.size() == kAudioEventCapacity);
    for (std::size_t i = 0; i < events.size(); ++i) {
        CAPTURE(i);
        CHECK(events.at(i) == AudioEvent{.kind = AudioEventKind::LateBlock,
                                         .count = 1,
                                         .load = 1.1,
                                         .duration = nanoseconds{11'000'000},
                                         .host_time = times.at(i)});
    }

    // With room again, the next is recorded, and the count stays.
    const Clock::time_point next = late_block();
    CHECK(take_all(*engine) ==
          std::vector<AudioEvent>{AudioEvent{.kind = AudioEventKind::LateBlock,
                                             .count = 1,
                                             .load = 1.1,
                                             .duration = nanoseconds{11'000'000},
                                             .host_time = next}});
    CHECK(engine->dropped_events() == kCalls - kAudioEventCapacity);
}
