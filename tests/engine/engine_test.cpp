// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/engine.h>

#include <ks/engine/block.h>
#include <ks/engine/render_core.h>
#include <ks/engine/resume.h>
#include <ks/ipc/command.h>
#include <ks/ipc/layout.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/spsc_ring.h>
#include <ks/ipc/state.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace {

using ks::engine::Block;
using ks::engine::ClientStats;
using ks::engine::Engine;
using ks::engine::kMaxBlockFrames;
using ks::engine::kMaxClients;
using ks::engine::Released;
using ks::engine::Track;
using ks::ipc::CommandRing;
using ks::ipc::CommandSlot;
using ks::ipc::CommandType;

constexpr std::size_t kChannels = 4;
constexpr std::uint32_t kRate = 48'000;

/// Renders one block of `frames` frames at `rate` into scratch channels.
void render_block(Engine& engine, std::size_t frames = 64, std::uint32_t rate = kRate) {
    std::vector<std::vector<float>> out(kChannels, std::vector<float>(kMaxBlockFrames + 1, 0.0F));
    std::vector<std::span<float>> views;
    views.reserve(out.size());
    for (std::vector<float>& channel : out) {
        views.emplace_back(std::span{channel}.first(frames));
    }
    engine.process(Block{.channels = views,
                         .frames = frames,
                         .sample_rate = rate,
                         .host_time = {},
                         .resumed = false});
}

/// A crossfader command, encoded by hand.
CommandSlot crossfader(std::uint64_t sequence, double position) {
    CommandSlot slot{};
    slot.sequence = sequence;
    slot.type = static_cast<std::uint32_t>(CommandType::SetCrossfader);
    slot.payload[0] = std::bit_cast<std::uint64_t>(position);
    return slot;
}

/// A ring on the heap, with its producer.
struct Ring {
    std::unique_ptr<CommandRing> ring = std::make_unique<CommandRing>();
    ks::ipc::RingProducer<CommandSlot, ks::ipc::kCommandRingCapacity> producer{*ring};
};

/// A silent stereo track that owns its samples.
struct TestTrack {
    explicit TestTrack(std::uint32_t rate = kRate) : track{samples, rate} {}

    std::vector<float> samples = std::vector<float>(std::size_t{2} * 4'800, 0.0F);
    Track track;
};

/// The track the next release points to, or null if there's none.
const Track* collect_track(Engine& engine) {
    return engine.collect().value_or(Released{}).track;
}

/// A command with one argument, encoded by hand.
CommandSlot command(std::uint64_t sequence, CommandType type, std::uint32_t target,
                    std::uint64_t argument) {
    CommandSlot slot{};
    slot.sequence = sequence;
    slot.type = static_cast<std::uint32_t>(type);
    slot.target = target;
    slot.payload[0] = argument;
    return slot;
}

/// Renders `blocks` 64-frame blocks of `engine` through a render core, and
/// returns every sample, channel by channel within each block. A play
/// command for deck 0 is pushed to `ring` before the first block, and a rate
/// command before the third.
std::vector<float> render_script(Engine& engine, Ring& ring, std::size_t blocks) {
    ks::engine::RenderCore core{engine};
    std::vector<std::vector<float>> out(kChannels, std::vector<float>(64, 0.0F));
    std::vector<std::span<float>> views(out.begin(), out.end());
    std::vector<float> rendered;
    REQUIRE(ring.producer.try_push(command(1, CommandType::SetPlaying, 0, 1)));
    for (std::size_t i = 0; i < blocks; ++i) {
        if (i == 2) {
            REQUIRE(ring.producer.try_push(
                command(2, CommandType::SetRate, 0, std::bit_cast<std::uint64_t>(1.25))));
        }
        REQUIRE(core.try_render(Block{.channels = views,
                                      .frames = 64,
                                      .sample_rate = kRate,
                                      .host_time = {},
                                      .resumed = false}) == ks::engine::RenderResult::Rendered);
        for (const std::vector<float>& channel : out) {
            rendered.insert(rendered.end(), channel.begin(), channel.end());
        }
    }
    return rendered;
}

/// Reads a seqlock that no thread is writing.
template <typename T>
T read(ks::ipc::Seqlock<T>& lock) {
    const std::optional<T> value = ks::ipc::SeqlockReader{lock}.try_read();
    REQUIRE(value.has_value());
    return value.value_or(T{});
}

/// A track whose samples are a ramp, completely decoded.
struct RampTrack {
    RampTrack() {
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i] = (static_cast<float>(i % 200) / 200.0F) - 0.5F;
        }
        track.publish(track.capacity(), true);
    }

    std::vector<float> samples = std::vector<float>(std::size_t{2} * 4'800, 0.0F);
    Track track{samples, kRate, 7};
};

} // namespace

TEST_CASE("Engine: a duplicate load releases the older reference at once", "[engine]") {
    auto engine = std::make_unique<Engine>();
    const TestTrack first;
    REQUIRE(engine->load(0, first.track));
    REQUIRE(engine->load(0, first.track));
    render_block(*engine);
    REQUIRE(collect_track(*engine) == &first.track);
    REQUIRE_FALSE(engine->collect().has_value());
    REQUIRE(engine->mixer().deck(0).track() == &first.track);

    REQUIRE(engine->unload(0));
    REQUIRE_FALSE(engine->load(ks::engine::kDecks, first.track));
    REQUIRE_FALSE(engine->unload(ks::engine::kDecks));
    render_block(*engine);
    REQUIRE(collect_track(*engine) == &first.track);
    REQUIRE_FALSE(engine->collect().has_value());
}

TEST_CASE("Engine: a refused load is released in the same block", "[engine]") {
    auto engine = std::make_unique<Engine>();
    const TestTrack refused{1'000};
    REQUIRE(engine->load(0, refused.track));
    render_block(*engine);
    REQUIRE(collect_track(*engine) == &refused.track);
    REQUIRE_FALSE(engine->collect().has_value());
    REQUIRE(engine->mixer().deck(0).track() == nullptr);
}

TEST_CASE("Engine: a burst of different tracks applies in one block", "[engine]") {
    auto engine = std::make_unique<Engine>();
    // A paused deck replaces each load at once, so it lets go of all but the
    // last, in load order, in the same block.
    const std::array<TestTrack, 12> tracks;
    for (const TestTrack& track : tracks) {
        REQUIRE(engine->load(0, track.track));
    }
    REQUIRE(engine->unload(1));
    render_block(*engine);
    REQUIRE(engine->mixer().deck(0).track() == &tracks.back().track);
    for (std::size_t i = 0; i + 1 < tracks.size(); ++i) {
        REQUIRE(collect_track(*engine) == &tracks.at(i).track);
    }
    REQUIRE_FALSE(engine->collect().has_value());

    REQUIRE(engine->unload(0));
    render_block(*engine);
    REQUIRE(collect_track(*engine) == &tracks.back().track);
    REQUIRE_FALSE(engine->collect().has_value());
}

TEST_CASE("Engine: loads wait only while releases wait, and keep their order", "[engine]") {
    auto engine = std::make_unique<Engine>();
    // Alternating two tracks on a paused deck releases one reference per
    // load, in load order: first, second, first, ...
    const TestTrack first;
    const TestTrack second;
    const std::array<const Track*, 2> alternate{&first.track, &second.track};
    std::size_t given = 0;
    std::size_t blocks = 0;
    bool refused = false;
    for (; blocks < 20 && !refused; ++blocks) {
        for (std::size_t i = 0; i < ks::engine::kHandoverCapacity; ++i) {
            if (!engine->load(0, *alternate.at(given % 2))) {
                refused = true;
                break;
            }
            ++given;
        }
        render_block(*engine);
    }
    // Nothing waits until the retire ring is full and the release queue has
    // filled behind it: two full give rings apply before any load waits.
    REQUIRE(refused);
    REQUIRE(given > 2 * ks::engine::kHandoverCapacity);

    std::size_t released = 0;
    for (std::size_t round = 0; round < 100 && released + 1 < given; ++round) {
        while (const std::optional<Released> next = engine->collect()) {
            REQUIRE(next.value_or(Released{}).track == alternate.at(released % 2));
            ++released;
        }
        render_block(*engine);
    }
    REQUIRE(released + 1 == given);
    REQUIRE(engine->mixer().deck(0).track() == alternate.at((given - 1) % 2));
}

TEST_CASE("Engine: an invalid block does nothing at all", "[engine]") {
    auto engine = std::make_unique<Engine>();
    Ring client;
    REQUIRE(engine->attach(*client.ring) == std::optional<std::size_t>{0});
    REQUIRE(client.producer.try_push(crossfader(1, 0.25)));

    render_block(*engine, 0);
    render_block(*engine, kMaxBlockFrames + 1);
    render_block(*engine, 64, 0);
    REQUIRE(engine->client_stats(0) == ClientStats{});
    REQUIRE(engine->mixer().settings().crossfader == 0.5);

    // The next valid block applies the attach, then reads the command.
    render_block(*engine, kMaxBlockFrames);
    REQUIRE(engine->client_stats(0).attached);
    REQUIRE(engine->client_stats(0).commands_read == 1);
    REQUIRE(engine->mixer().settings().crossfader == 0.25);
}

TEST_CASE("Engine: detach needs an attached slot", "[engine]") {
    auto engine = std::make_unique<Engine>();
    Ring client;
    REQUIRE_FALSE(engine->detach(0));
    REQUIRE_FALSE(engine->detach(kMaxClients));
    REQUIRE(engine->attach(*client.ring) == std::optional<std::size_t>{0});
    REQUIRE_FALSE(engine->detach(1));
    REQUIRE(engine->detach(0));
    REQUIRE_FALSE(engine->detach(0));
    REQUIRE(engine->client_stats(kMaxClients) == ClientStats{});

    // The slot stays in use until the release is collected.
    render_block(*engine);
    Ring other;
    REQUIRE(engine->attach(*other.ring) == std::optional<std::size_t>{1});
    // A missing release reads as a null ring, which fails the check.
    const Released released = engine->collect().value_or(Released{});
    REQUIRE(released.ring == client.ring.get());
    REQUIRE(released.track == nullptr);
    REQUIRE_FALSE(engine->collect().has_value());
    REQUIRE(engine->attach(*client.ring) == std::optional<std::size_t>{0});
}

TEST_CASE("Engine: rings are released in the order they're detached", "[engine]") {
    auto engine = std::make_unique<Engine>();
    std::array<Ring, kMaxClients> clients;
    for (std::size_t i = 0; i < kMaxClients; ++i) {
        REQUIRE(engine->attach(*clients.at(i).ring) == std::optional<std::size_t>{i});
    }
    render_block(*engine);
    constexpr std::array<std::size_t, kMaxClients> kOrder{5, 2, 7, 0, 3, 6, 1, 4};
    for (const std::size_t slot : kOrder) {
        REQUIRE(engine->detach(slot));
    }
    REQUIRE_FALSE(engine->collect().has_value());
    render_block(*engine);
    for (const std::size_t slot : kOrder) {
        const Released released = engine->collect().value_or(Released{});
        REQUIRE(released.ring == clients.at(slot).ring.get());
        REQUIRE_FALSE(engine->client_stats(slot).attached);
    }
    REQUIRE_FALSE(engine->collect().has_value());
}

TEST_CASE("Engine: a corrupt ring is read no further", "[engine]") {
    auto engine = std::make_unique<Engine>();
    Ring client;
    REQUIRE(engine->attach(*client.ring) == std::optional<std::size_t>{0});
    REQUIRE(client.producer.try_push(crossfader(7, 0.75)));
    render_block(*engine);
    ks::ipc::store_shared(client.ring->head, 1'000'000, std::memory_order_release);
    render_block(*engine);
    REQUIRE(engine->client_stats(0) == ClientStats{.attached = true,
                                                   .corrupt = true,
                                                   .commands_read = 1,
                                                   .last_sequence = 7,
                                                   .rejected = 0,
                                                   .last_rejection = std::nullopt});

    // Detaching still releases it, and the next client starts clean.
    REQUIRE(engine->detach(0));
    render_block(*engine);
    REQUIRE(engine->collect().has_value());
    Ring next;
    REQUIRE(engine->attach(*next.ring) == std::optional<std::size_t>{0});
    render_block(*engine);
    REQUIRE(engine->client_stats(0) == ClientStats{.attached = true});
}

TEST_CASE("Engine: a state block doesn't change the output", "[engine][state]") {
    const RampTrack track;
    const auto state = std::make_unique<ks::ipc::EngineState>();
    const auto plain = std::make_unique<Engine>();
    const auto publishing = std::make_unique<Engine>(*state);
    Ring plain_ring;
    Ring publishing_ring;
    for (Engine* const engine : {plain.get(), publishing.get()}) {
        REQUIRE(engine->load(0, track.track));
    }
    REQUIRE(plain->attach(*plain_ring.ring).has_value());
    REQUIRE(publishing->attach(*publishing_ring.ring).has_value());

    const std::vector<float> without = render_script(*plain, plain_ring, 8);
    const std::vector<float> with = render_script(*publishing, publishing_ring, 8);
    REQUIRE(without == with);
    // The script makes sound, so the comparison means something.
    REQUIRE(std::ranges::any_of(with, [](float sample) { return sample != 0.0F; }));
    REQUIRE(state->status.sequence == 16);
}

TEST_CASE("Engine: each rendered block writes every seqlock once", "[engine][state]") {
    const auto state = std::make_unique<ks::ipc::EngineState>();
    const auto engine = std::make_unique<Engine>(*state);
    ks::engine::RenderCore core{*engine};
    std::vector<std::vector<float>> out(kChannels, std::vector<float>(64, 0.0F));
    std::vector<std::span<float>> views(out.begin(), out.end());
    const Block block{
        .channels = views, .frames = 64, .sample_rate = kRate, .host_time = {}, .resumed = false};

    const auto sequences_are = [&](std::uint64_t expected) {
        for (const auto& deck : state->decks) {
            REQUIRE(deck.sequence == expected);
        }
        REQUIRE(state->mixer.sequence == expected);
        REQUIRE(state->status.sequence == expected);
    };

    // Nothing is written before the first block.
    sequences_are(0);
    REQUIRE(read(state->status).heartbeat == 0);

    // A skipped block publishes nothing.
    core.activate(ks::engine::DriverId{1});
    REQUIRE(core.try_render(block) == ks::engine::RenderResult::Skipped);
    sequences_are(0);
    core.activate(ks::engine::kDefaultDriver);

    for (std::uint64_t n = 1; n <= 3; ++n) {
        REQUIRE(core.try_render(block) == ks::engine::RenderResult::Rendered);
        sequences_are(2 * n);
        const ks::ipc::EngineStatus status = read(state->status);
        REQUIRE(status.heartbeat == n);
        REQUIRE(status.frames_rendered == 64 * n);
        REQUIRE(status.sample_rate == kRate);
        const ks::ipc::MixerState mixer = read(state->mixer);
        REQUIRE(mixer.heartbeat == n);
        REQUIRE(mixer.bus_cue == 2);
    }
}

TEST_CASE("Engine: a deck's block describes its track", "[engine][state]") {
    const RampTrack track;
    const auto state = std::make_unique<ks::ipc::EngineState>();
    const auto engine = std::make_unique<Engine>(*state);
    ks::engine::RenderCore core{*engine};
    std::vector<std::vector<float>> out(kChannels, std::vector<float>(64, 0.0F));
    std::vector<std::span<float>> views(out.begin(), out.end());
    const auto start = std::chrono::steady_clock::time_point{std::chrono::seconds{3}};
    REQUIRE(engine->load(1, track.track));
    REQUIRE(core.try_render(Block{.channels = views,
                                  .frames = 64,
                                  .sample_rate = kRate,
                                  .host_time = start,
                                  .output_latency = std::chrono::milliseconds{5},
                                  .resumed = false}) == ks::engine::RenderResult::Rendered);

    const ks::ipc::DeckState empty = read(state->decks.at(0));
    REQUIRE(empty.flags == 0);
    REQUIRE(empty.track_id == 0);
    REQUIRE(empty.track_frames == 0);

    const ks::ipc::DeckState loaded = read(state->decks.at(1));
    REQUIRE(loaded.heartbeat == 1);
    REQUIRE(loaded.flags == (ks::ipc::kDeckLoaded | ks::ipc::kDeckComplete));
    REQUIRE(loaded.track_id == 7);
    REQUIRE(loaded.track_sample_rate == kRate);
    REQUIRE(loaded.track_frames == 4'800);
    REQUIRE(loaded.frames_ready == 4'800);
    REQUIRE(loaded.rate == std::bit_cast<std::uint64_t>(engine->mixer().deck(1).rate()));
    const auto end = start + ks::engine::frames_to_duration(64, kRate);
    const auto end_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count();
    REQUIRE(loaded.host_time == static_cast<std::uint64_t>(end_ns));
    REQUIRE(loaded.output_latency == 5'000'000);
}

TEST_CASE("Engine: the status acknowledges each client slot", "[engine][state]") {
    const auto state = std::make_unique<ks::ipc::EngineState>();
    const auto engine = std::make_unique<Engine>(*state);
    ks::engine::RenderCore core{*engine};
    std::vector<std::vector<float>> out(kChannels, std::vector<float>(64, 0.0F));
    std::vector<std::span<float>> views(out.begin(), out.end());
    const Block block{
        .channels = views, .frames = 64, .sample_rate = kRate, .host_time = {}, .resumed = false};
    Ring ring;
    REQUIRE(engine->attach(*ring.ring) == std::optional<std::size_t>{0});
    REQUIRE(ring.producer.try_push(crossfader(5, 0.25)));
    // Deck 9 doesn't exist: BadTarget, the second Rejection.
    REQUIRE(ring.producer.try_push(command(6, CommandType::SetPlaying, 9, 1)));
    REQUIRE(core.try_render(block) == ks::engine::RenderResult::Rendered);

    const ks::ipc::EngineStatus status = read(state->status);
    const ks::ipc::ClientAck& ack = status.clients.at(0);
    REQUIRE(ack.flags == ks::ipc::kClientAttached);
    REQUIRE(ack.commands_read == 2);
    REQUIRE(ack.last_sequence == 6);
    REQUIRE(ack.rejected == 1);
    REQUIRE(engine->client_stats(0).last_rejection == ks::engine::Rejection::BadTarget);
    const auto bad_target = static_cast<std::uint64_t>(ks::engine::Rejection::BadTarget);
    REQUIRE(ack.last_rejection == bad_target + 1);
    // Detached slots read as zero.
    for (std::size_t i = 1; i < kMaxClients; ++i) {
        REQUIRE(status.clients.at(i).flags == 0);
        REQUIRE(status.clients.at(i).commands_read == 0);
    }
}

TEST_CASE("Engine: events taken while the audio thread records them", "[engine][events]") {
    // The audio thread records one late block per call, as fast as it can,
    // while the control thread takes them. Under tsan, this checks that the
    // event ring and the dropped count are shared safely.
    constexpr std::uint64_t kCalls = 100'000;
    auto engine = std::make_unique<Engine>();
    std::array<float, 64> left{};
    std::array<float, 64> right{};
    const std::array<std::span<float>, 2> views{std::span{left}, std::span{right}};
    std::atomic<bool> done{false};

    std::thread audio{[&] {
        for (std::uint64_t call = 1; call <= kCalls; ++call) {
            const Block block{.channels = views,
                              .frames = left.size(),
                              .sample_rate = kRate,
                              .host_time =
                                  std::chrono::steady_clock::time_point{
                                      std::chrono::nanoseconds{static_cast<std::int64_t>(call)}},
                              .resumed = false};
            engine->rendered(block,
                             ks::engine::RenderStats{.blocks_rendered = call, .late_blocks = call});
        }
        done.store(true, std::memory_order_release);
    }};

    std::uint64_t taken = 0;
    std::int64_t last = 0;
    bool in_order = true;
    for (;;) {
        // Read before taking: once it's set, every event is in the ring or
        // counted as dropped.
        const bool finished = done.load(std::memory_order_acquire);
        while (const std::optional<ks::engine::AudioEvent> event = engine->take_event()) {
            const std::int64_t time = event->host_time.time_since_epoch().count();
            in_order = in_order && time > last && event->count == 1;
            last = time;
            ++taken;
        }
        if (finished) {
            break;
        }
    }
    audio.join();
    CHECK(in_order);
    CHECK(taken + engine->dropped_events() == kCalls);
}

TEST_CASE("Engine: commands stay in their ring until a snapshot holds them", "[engine][state]") {
    // An engine that dies between reading a command and publishing the
    // snapshot that includes it leaves the command to the next engine.
    const auto state = std::make_unique<ks::ipc::EngineState>();
    Ring client;
    const auto tail = [&] {
        return ks::ipc::load_shared(client.ring->tail, std::memory_order_acquire);
    };
    {
        const auto first = std::make_unique<Engine>(*state);
        ks::engine::RenderCore core{*first};
        REQUIRE(first->attach(*client.ring) == std::optional<std::size_t>{0});
        std::vector<std::vector<float>> out(kChannels, std::vector<float>(64, 0.0F));
        std::vector<std::span<float>> views(out.begin(), out.end());
        const Block block{.channels = views,
                          .frames = 64,
                          .sample_rate = kRate,
                          .host_time = {},
                          .resumed = false};
        REQUIRE(core.try_render(block) == ks::engine::RenderResult::Rendered);
        REQUIRE(client.producer.try_push(crossfader(1, 0.25)));
        REQUIRE(core.try_render(block) == ks::engine::RenderResult::Rendered);
        // Published, then released.
        CHECK(read(state->mixer).crossfader == std::bit_cast<std::uint64_t>(0.25));
        CHECK(tail() == 1);

        // This one dies after it's read, before rendered() publishes it.
        REQUIRE(client.producer.try_push(crossfader(2, 0.75)));
        first->process(block);
        REQUIRE(first->mixer().settings().crossfader == 0.75);
        CHECK(tail() == 1);
        CHECK(read(state->mixer).crossfader == std::bit_cast<std::uint64_t>(0.25));
    }

    // The next engine takes the ring over where the snapshot left it, and
    // applies the command again.
    // No tracks: only the mixer resumes.
    const auto second = std::make_unique<Engine>(*state, std::span<const Track* const>{});
    REQUIRE(second->mixer().settings().crossfader == 0.25);
    REQUIRE(second->attach_at(0, *client.ring));
    render_block(*second);
    CHECK(second->client_stats(0).last_sequence == 2);
    CHECK(second->mixer().settings().crossfader == 0.75);
    // A caller that never reports blocks rendered gets them released by the
    // next block.
    CHECK(tail() == 1);
    render_block(*second);
    CHECK(tail() == 2);
}

TEST_CASE("Engine: a handover is held once a snapshot holds it", "[engine][state]") {
    // The control thread answers a load or an unload only once it's held, so
    // that an engine that dies first leaves it unanswered rather than lost.
    const auto state = std::make_unique<ks::ipc::EngineState>();
    const auto engine = std::make_unique<Engine>(*state);
    std::vector<float> samples(std::size_t{2} * 4'800, 0.0F);
    const Track track{samples, kRate, 7};
    std::vector<std::vector<float>> out(kChannels, std::vector<float>(64, 0.0F));
    std::vector<std::span<float>> views(out.begin(), out.end());
    const Block block{
        .channels = views, .frames = 64, .sample_rate = kRate, .host_time = {}, .resumed = false};
    const auto snapshot_track = [&] {
        const std::optional<ks::engine::Snapshot> snapshot = ks::engine::read_snapshot(*state);
        return snapshot ? snapshot->decks.front().track_id : 0;
    };
    CHECK(engine->handovers_given() == 0);
    CHECK(engine->handovers_held() == 0);

    REQUIRE(engine->load(0, track));
    CHECK(engine->handovers_given() == 1);
    // Applied, but no snapshot holds it yet.
    engine->process(block);
    REQUIRE(engine->mixer().deck(0).track() == &track);
    CHECK(engine->handovers_held() == 0);
    CHECK(snapshot_track() == 0);
    // rendered() writes the snapshot, and then it's held.
    engine->rendered(block, ks::engine::RenderStats{.blocks_rendered = 1});
    CHECK(snapshot_track() == 7);
    CHECK(engine->handovers_held() == 1);

    REQUIRE(engine->unload(0));
    CHECK(engine->handovers_given() == 2);
    engine->process(block);
    REQUIRE(engine->mixer().deck(0).track() == nullptr);
    CHECK(engine->handovers_held() == 1);
    engine->rendered(block, ks::engine::RenderStats{.blocks_rendered = 2});
    CHECK(snapshot_track() == 0);
    CHECK(engine->handovers_held() == 2);
}

TEST_CASE("Engine: a handover that waits is held only once applied", "[engine]") {
    // Without a state block there's no snapshot to wait for: a handover is
    // held from the next block on.
    auto engine = std::make_unique<Engine>();
    const TestTrack first;
    const TestTrack second;
    const std::array<const Track*, 2> alternate{&first.track, &second.track};
    std::uint64_t given = 0;
    bool refused = false;
    for (std::size_t blocks = 0; blocks < 20 && !refused; ++blocks) {
        for (std::size_t i = 0; i < ks::engine::kHandoverCapacity; ++i) {
            if (!engine->load(0, *alternate.at(given % 2))) {
                refused = true;
                break;
            }
            ++given;
        }
        render_block(*engine);
    }
    REQUIRE(refused);
    CHECK(engine->handovers_given() == given);
    // Nothing collected: the waiting load holds up everything behind it.
    render_block(*engine);
    const std::uint64_t waiting = engine->handovers_held();
    CHECK(waiting < given);
    render_block(*engine);
    CHECK(engine->handovers_held() == waiting);

    // Collecting lets every load apply, each counted once.
    for (std::size_t round = 0; round < 100 && engine->handovers_held() < given; ++round) {
        while (engine->collect().has_value()) {
        }
        render_block(*engine);
    }
    render_block(*engine);
    CHECK(engine->handovers_held() == given);
}
