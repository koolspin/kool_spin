// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/engine.h>

#include <ks/engine/block.h>
#include <ks/ipc/command.h>
#include <ks/ipc/layout.h>
#include <ks/ipc/spsc_ring.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
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

TEST_CASE("Engine: a deck with no room makes loads wait, in order", "[engine]") {
    auto engine = std::make_unique<Engine>();
    // Fill the retire ring with duplicate releases from deck 1.
    const TestTrack filler;
    for (std::size_t i = 0; i < ks::engine::kHandoverCapacity; ++i) {
        REQUIRE(engine->load(1, filler.track));
    }
    render_block(*engine);
    REQUIRE(engine->load(1, filler.track));
    render_block(*engine);

    // Deck 0 takes four references; the fifth distinct track waits, and so
    // does everything given after it.
    const std::array<TestTrack, 6> tracks;
    for (const TestTrack& track : tracks) {
        REQUIRE(engine->load(0, track.track));
    }
    render_block(*engine);
    render_block(*engine);
    REQUIRE(engine->mixer().deck(0).track() == &tracks.at(3).track);

    for (std::size_t i = 0; i < ks::engine::kHandoverCapacity; ++i) {
        REQUIRE(collect_track(*engine) == &filler.track);
    }
    REQUIRE_FALSE(engine->collect().has_value());
    // The first three releases free their slots after this block's handovers.
    render_block(*engine);
    for (std::size_t i = 0; i < 3; ++i) {
        REQUIRE(collect_track(*engine) == &tracks.at(i).track);
    }
    REQUIRE_FALSE(engine->collect().has_value());
    REQUIRE(engine->mixer().deck(0).track() == &tracks.at(3).track);

    // The fourth and fifth let go in the same block, released in slot order.
    render_block(*engine);
    const Track* const one = collect_track(*engine);
    const Track* const two = collect_track(*engine);
    REQUIRE(((one == &tracks.at(3).track && two == &tracks.at(4).track) ||
             (one == &tracks.at(4).track && two == &tracks.at(3).track)));
    REQUIRE_FALSE(engine->collect().has_value());
    REQUIRE(engine->mixer().deck(0).track() == &tracks.at(5).track);
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
