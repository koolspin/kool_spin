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

} // namespace

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
