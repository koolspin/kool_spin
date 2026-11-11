// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/commands/spec.md: the engine reads commands, and
// hands tracks and rings to and from the audio thread.

#include <ks/engine/engine.h>

#include <ks/engine/command.h>
#include <ks/engine/interpolator.h>
#include <ks/engine/mixer.h>
#include <ks/engine/track.h>
#include <ks/ipc/command.h>
#include <ks/ipc/layout.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <vector>

namespace {

using ks::engine::BusMap;
using ks::engine::ClientStats;
using ks::engine::Command;
using ks::engine::CrossfaderSide;
using ks::engine::Engine;
using ks::engine::Interpolator;
using ks::engine::kDecks;
using ks::engine::Mixer;
using ks::engine::MixerSettings;
using ks::engine::Rejection;
using ks::engine::Track;
using ks::engine::test::assign;
using ks::engine::test::bus_map;
using ks::engine::test::channel_cue;
using ks::engine::test::Client;
using ks::engine::test::constant_track;
using ks::engine::test::deck_command;
using ks::engine::test::encode;
using ks::engine::test::global_command;
using ks::engine::test::Ledger;
using ks::engine::test::music_track;
using ks::engine::test::Outputs;
using ks::engine::test::OwnedTrack;
using ks::engine::test::playing;
using ks::engine::test::process_into;
using ks::engine::test::render;
using ks::ipc::CommandSlot;
using ks::ipc::CommandType;

constexpr std::size_t kBlock = 64;
/// The deck's 5 ms fade, in frames at 48 kHz.
constexpr std::size_t kFadeFrames = 240;

/// Renders one 64-frame block of 4 channels, and discards it.
void render_block(Engine& engine) {
    static_cast<void>(render(engine, kBlock));
}

/// Whether two renders are identical, bit for bit. Reports the first
/// difference.
bool identical(const Outputs& a, const Outputs& b) {
    for (std::size_t c = 0; c < a.size(); ++c) {
        for (std::size_t f = 0; f < a.at(c).size(); ++f) {
            if (std::bit_cast<std::uint32_t>(a.at(c).at(f)) !=
                std::bit_cast<std::uint32_t>(b.at(c).at(f))) {
                UNSCOPED_INFO("channel " << c << ", frame " << f << " differs");
                return false;
            }
        }
    }
    return true;
}

bool any_sound(const Outputs& out) {
    for (const auto& channel : out) {
        for (const float sample : channel) {
            if (sample != 0.0F) {
                return true;
            }
        }
    }
    return false;
}

/// Renders blocks until `track` has no references outstanding, collecting
/// after each. Returns the frames rendered, or nothing if it took more than
/// 20 blocks. While it waits, deck `deck` must still hold the track.
std::optional<std::size_t> frames_until_released(Engine& engine, Ledger& ledger, const Track& track,
                                                 std::size_t deck) {
    for (std::size_t block = 1; block <= 20; ++block) {
        render_block(engine);
        if (!ledger.collect_all(engine)) {
            return std::nullopt;
        }
        if (ledger.outstanding(track) == 0) {
            return block * kBlock;
        }
        if (!engine.mixer().deck(deck).holds(track)) {
            UNSCOPED_INFO("the deck let go of the track without releasing it");
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("Engine: commands do what direct calls do", "[commands][engine]") {
    using enum CommandType;
    const auto first = music_track(0);
    const auto second = music_track(7'777);
    const auto third = constant_track(0.15F);
    const auto fourth = constant_track(0.2F);
    const std::array<const Track*, kDecks> tracks{&first->track(), &second->track(),
                                                  &third->track(), &fourth->track()};

    auto engine = std::make_unique<Engine>();
    const Interpolator interpolator;
    Mixer direct{interpolator};
    Client client;
    REQUIRE(engine->attach(client.ring()) == std::optional<std::size_t>{0});
    for (std::size_t deck = 0; deck < kDecks; ++deck) {
        REQUIRE(engine->load(deck, *tracks.at(deck)));
        REQUIRE(direct.deck(deck).load(*tracks.at(deck)));
    }

    // Every command type, sent before the block it's listed with.
    struct Step {
        std::size_t block;
        Command command;
    };
    const std::vector<Step> script{
        {.block = 0, .command = playing(0, true)},
        {.block = 0, .command = playing(1, true)},
        {.block = 0, .command = playing(2, true)},
        {.block = 0, .command = assign(0, CrossfaderSide::A)},
        {.block = 0, .command = assign(1, CrossfaderSide::B)},
        {.block = 0, .command = channel_cue(2, true)},
        {.block = 10, .command = deck_command(SetRate, 1, 1.06)},
        {.block = 20, .command = global_command(SetCrossfader, 0.3)},
        {.block = 30, .command = deck_command(SetFader, 3, 0.5)},
        {.block = 30, .command = deck_command(SetTrim, 0, -3.0)},
        {.block = 40, .command = global_command(SetCrossfaderCurve, 0.2)},
        {.block = 50, .command = global_command(SetCueMix, 0.4)},
        {.block = 50, .command = global_command(SetHeadphoneLevel, -6.0)},
        {.block = 60, .command = deck_command(Seek, 0, 30'000.0)},
        {.block = 70, .command = deck_command(SetCuePoint, 1, 1'000.0)},
        {.block = 70, .command = global_command(SetMainLevel, -2.0)},
        {.block = 80, .command = playing(2, false)},
        {.block = 80,
         .command = deck_command(SetTrim, 1, -std::numeric_limits<double>::infinity())},
        {.block = 85, .command = playing(3, true)},
        {.block = 90, .command = bus_map(BusMap{.main = 2, .cue = 0})},
        {.block = 100, .command = deck_command(SetRate, 0, -1.0)},
        {.block = 110, .command = deck_command(SetFader, 0, 2.0)},
    };

    constexpr std::size_t kBlocks = 120;
    Outputs through_engine(4, std::vector<float>(kBlocks * kBlock, 0.0F));
    Outputs called(4, std::vector<float>(kBlocks * kBlock, 0.0F));
    std::size_t next = 0;
    for (std::size_t block = 0; block < kBlocks; ++block) {
        while (next < script.size() && script.at(next).block == block) {
            REQUIRE(client.send(script.at(next).command));
            ks::engine::test::call_directly(direct, script.at(next).command);
            ++next;
        }
        process_into(*engine, through_engine, block * kBlock, kBlock);
        process_into(direct, called, block * kBlock, kBlock);
    }

    REQUIRE(any_sound(called));
    REQUIRE(identical(through_engine, called));
    REQUIRE(engine->mixer().settings() == direct.settings());
    for (std::size_t deck = 0; deck < kDecks; ++deck) {
        CAPTURE(deck);
        const ks::engine::Deck& actual = engine->mixer().deck(deck);
        const ks::engine::Deck& expected = direct.deck(deck);
        REQUIRE(actual.track() == expected.track());
        REQUIRE(actual.playing() == expected.playing());
        REQUIRE(actual.rate() == expected.rate());
        REQUIRE(actual.position() == expected.position());
        REQUIRE(actual.cue() == expected.cue());
    }
    REQUIRE(engine->client_stats(0).commands_read == script.size());
    REQUIRE(engine->client_stats(0).rejected == 0);
}

TEST_CASE("Engine: every command read is counted", "[commands][engine]") {
    using enum CommandType;
    constexpr std::uint64_t kQuietNaN = 0x7FF8'0000'0000'0000ULL;
    auto engine = std::make_unique<Engine>();
    Client client;
    REQUIRE(engine->attach(client.ring()) == std::optional<std::size_t>{0});
    render_block(*engine);
    REQUIRE(engine->client_stats(0) == ClientStats{.attached = true});
    REQUIRE(engine->client_stats(1) == ClientStats{});

    // Valid, unknown type, NaN, valid, bad target.
    CommandSlot unknown = encode(11, deck_command(SetFader, 0, 0.3));
    unknown.type = 99;
    CommandSlot nan = encode(12, deck_command(SetFader, 1, 0.25));
    nan.payload[0] = kQuietNaN;
    CommandSlot far = encode(14, deck_command(SetFader, 2, 0.1));
    far.target = 7;
    REQUIRE(client.send_slot(encode(10, deck_command(SetFader, 0, 0.5))));
    REQUIRE(client.send_slot(unknown));
    REQUIRE(client.send_slot(nan));
    REQUIRE(client.send_slot(encode(13, global_command(SetCrossfader, 0.75))));
    REQUIRE(client.send_slot(far));
    render_block(*engine);

    REQUIRE(engine->client_stats(0) == ClientStats{.attached = true,
                                                   .corrupt = false,
                                                   .commands_read = 5,
                                                   .last_sequence = 14,
                                                   .rejected = 3,
                                                   .last_rejection = Rejection::BadTarget});
    const MixerSettings& settings = engine->mixer().settings();
    REQUIRE(settings.channels.at(0).fader == 0.5);
    REQUIRE(settings.channels.at(1).fader == 1.0);
    REQUIRE(settings.channels.at(2).fader == 1.0);
    REQUIRE(settings.crossfader == 0.75);

    // Rejected commands change no setting.
    const MixerSettings before = engine->mixer().settings();
    CommandSlot infinite = encode(21, deck_command(SetTrim, 0, 0.0));
    infinite.payload[0] = ks::engine::test::bits_of(std::numeric_limits<double>::infinity());
    CommandSlot flag = encode(22, playing(0, true));
    flag.payload[0] = 2;
    REQUIRE(client.send_slot(CommandSlot{.sequence = 20, .type = 0, .target = 0, .payload = {}}));
    REQUIRE(client.send_slot(infinite));
    REQUIRE(client.send_slot(flag));
    render_block(*engine);
    REQUIRE(engine->mixer().settings() == before);
    REQUIRE(engine->client_stats(0) == ClientStats{.attached = true,
                                                   .corrupt = false,
                                                   .commands_read = 8,
                                                   .last_sequence = 22,
                                                   .rejected = 6,
                                                   .last_rejection = Rejection::BadValue});
}

TEST_CASE("Engine: rings are read fairly, in slot order", "[commands][engine]") {
    using enum CommandType;
    auto engine = std::make_unique<Engine>();
    Client first;
    Client second;
    REQUIRE(engine->attach(first.ring()) == std::optional<std::size_t>{0});
    REQUIRE(engine->attach(second.ring()) == std::optional<std::size_t>{1});

    // 32 commands from each ring per block.
    for (std::size_t i = 0; i < 100; ++i) {
        const double position = static_cast<double>(i) / 100.0;
        REQUIRE(first.send(deck_command(SetFader, 0, position)));
        REQUIRE(second.send(deck_command(SetFader, 1, position)));
    }
    for (const std::uint64_t read : {32U, 64U, 96U, 100U}) {
        render_block(*engine);
        CAPTURE(read);
        REQUIRE(engine->client_stats(0).commands_read == read);
        REQUIRE(engine->client_stats(1).commands_read == read);
    }
    REQUIRE(engine->mixer().settings().channels.at(0).fader == 0.99);
    REQUIRE(engine->mixer().settings().channels.at(1).fader == 0.99);

    // The same control in the same block: the later slot wins, whichever
    // client sent first.
    REQUIRE(first.send(deck_command(SetFader, 2, 0.2)));
    REQUIRE(second.send(deck_command(SetFader, 2, 0.7)));
    render_block(*engine);
    REQUIRE(engine->mixer().settings().channels.at(2).fader == 0.7);
    REQUIRE(second.send(deck_command(SetFader, 2, 0.3)));
    REQUIRE(first.send(deck_command(SetFader, 2, 0.9)));
    render_block(*engine);
    REQUIRE(engine->mixer().settings().channels.at(2).fader == 0.3);

    // A corrupt ring is read no further; the other client carries on.
    ks::ipc::store_shared(first.ring().head, 1'000'000, std::memory_order_release);
    REQUIRE(second.send(deck_command(SetFader, 3, 0.4)));
    render_block(*engine);
    REQUIRE(engine->client_stats(0).corrupt);
    REQUIRE(engine->client_stats(0).attached);
    REQUIRE_FALSE(engine->client_stats(1).corrupt);
    REQUIRE(engine->mixer().settings().channels.at(3).fader == 0.4);
    const std::uint64_t read = engine->client_stats(0).commands_read;
    REQUIRE(first.send(deck_command(SetFader, 0, 0.1)));
    render_block(*engine);
    REQUIRE(engine->client_stats(0).commands_read == read);
    REQUIRE(engine->mixer().settings().channels.at(0).fader == 0.99);
}

TEST_CASE("Engine: loads hand tracks over and release them", "[commands][engine]") {
    auto engine = std::make_unique<Engine>();
    Client client;
    Ledger ledger;
    REQUIRE(engine->attach(client.ring()).has_value());
    const auto first = constant_track(0.05F);
    const auto second = constant_track(0.1F);

    // A loaded track plays after a play command.
    REQUIRE(engine->load(0, first->track()));
    ledger.loaded(first->track());
    REQUIRE(client.send(playing(0, true)));
    const Outputs out = render(*engine, 2'000);
    REQUIRE(ledger.collect_all(*engine));
    REQUIRE(ledger.track_releases() == 0);
    REQUIRE(engine->mixer().deck(0).track() == &first->track());
    REQUIRE(engine->mixer().deck(0).playing());
    REQUIRE(ks::engine::test::near(out.at(0).back(), 0.05));

    // Loading another fades the first out over 5 ms, then releases it.
    REQUIRE(engine->load(0, second->track()));
    ledger.loaded(second->track());
    const std::optional<std::size_t> fade =
        frames_until_released(*engine, ledger, first->track(), 0);
    REQUIRE(fade.has_value());
    CAPTURE(fade.value_or(0));
    REQUIRE(fade.value_or(0) >= kFadeFrames);
    REQUIRE(fade.value_or(0) <= kFadeFrames + kBlock);
    REQUIRE(engine->mixer().deck(0).track() == &second->track());
    REQUIRE(ledger.outstanding(second->track()) == 1);

    // Unloading a playing deck releases its track after the fade.
    REQUIRE(client.send(playing(0, true)));
    static_cast<void>(render(*engine, 2'000));
    REQUIRE(engine->unload(0));
    const std::optional<std::size_t> unload =
        frames_until_released(*engine, ledger, second->track(), 0);
    REQUIRE(unload.has_value());
    CAPTURE(unload.value_or(0));
    REQUIRE(unload.value_or(0) >= kFadeFrames);
    REQUIRE(unload.value_or(0) <= kFadeFrames + kBlock);
    REQUIRE(engine->mixer().deck(0).track() == nullptr);

    // A track the deck refuses is released in the same block.
    // 1 kHz is below the lowest rate a deck accepts.
    OwnedTrack refused{1'000, 1'000, [](std::uint64_t, std::size_t) { return 0.5F; }};
    REQUIRE(engine->load(1, refused.track()));
    ledger.loaded(refused.track());
    render_block(*engine);
    REQUIRE(ledger.collect_all(*engine));
    REQUIRE(ledger.outstanding(refused.track()) == 0);
    REQUIRE(engine->mixer().deck(1).track() == nullptr);
    REQUIRE(ledger.outstanding() == 0);
}

TEST_CASE("Engine: every reference is released once, after its deck lets go",
          "[commands][engine]") {
    auto engine = std::make_unique<Engine>();
    Client client;
    Ledger ledger;
    REQUIRE(engine->attach(client.ring()).has_value());
    const std::array<std::unique_ptr<OwnedTrack>, 3> tracks{constant_track(0.05F),
                                                            constant_track(0.1F), music_track(0)};

    std::mt19937_64 random{2'026};
    Outputs scratch(4, std::vector<float>(300, 0.0F));
    for (std::size_t op = 0; op < 1'000; ++op) {
        const std::uint64_t what = random() % 10;
        const std::size_t deck = random() % kDecks;
        const Track& track = tracks.at(random() % tracks.size())->track();
        if (what < 6) {
            if (engine->load(deck, track)) {
                ledger.loaded(track);
            }
        } else if (what < 8) {
            static_cast<void>(engine->unload(deck));
        } else {
            static_cast<void>(client.send(playing(deck, true)));
        }
        process_into(*engine, scratch, 0, 1 + (random() % 300));

        if (!ledger.collect_all(*engine)) {
            FAIL("operation " << op << ": a release didn't match an outstanding reference");
        }
        for (const auto& owned : tracks) {
            if (ledger.outstanding(owned->track()) != 0) {
                continue;
            }
            for (std::size_t d = 0; d < kDecks; ++d) {
                if (engine->mixer().deck(d).holds(owned->track())) {
                    FAIL("operation " << op << ": deck " << d
                                      << " holds a track whose references were all released");
                }
            }
        }
    }

    for (std::size_t deck = 0; deck < kDecks; ++deck) {
        REQUIRE(engine->unload(deck));
    }
    static_cast<void>(render(*engine, 1'000));
    REQUIRE(ledger.collect_all(*engine));
    REQUIRE(ledger.loads() >= 400);
    REQUIRE(ledger.outstanding() == 0);
    REQUIRE(ledger.track_releases() == ledger.loads());
}

TEST_CASE("Engine: full rings delay, but lose nothing", "[commands][engine]") {
    const auto first = constant_track(0.05F);
    const auto second = constant_track(0.1F);

    SECTION("the give ring holds 64 handovers, and one block applies them all") {
        auto engine = std::make_unique<Engine>();
        Ledger ledger;
        for (std::size_t i = 0; i < ks::engine::kHandoverCapacity; ++i) {
            REQUIRE(engine->load(0, first->track()));
            ledger.loaded(first->track());
        }
        REQUIRE_FALSE(engine->load(0, first->track()));
        REQUIRE_FALSE(engine->unload(0));
        Client client;
        REQUIRE_FALSE(engine->attach(client.ring()).has_value());

        // Duplicate references leave the deck in the same block, so none of
        // the 64 has to wait.
        render_block(*engine);
        for (std::size_t i = 0; i < ks::engine::kHandoverCapacity; ++i) {
            REQUIRE(engine->load(1, second->track()));
            ledger.loaded(second->track());
        }
        REQUIRE(ledger.collect_all(*engine));
        REQUIRE(ledger.outstanding(first->track()) == 1);

        render_block(*engine);
        REQUIRE(ledger.collect_all(*engine));
        REQUIRE(ledger.outstanding() == 2);
    }

    SECTION("with nothing collected, releases wait, then loads wait, in order") {
        auto engine = std::make_unique<Engine>();
        Ledger ledger;
        const std::array<const Track*, 2> alternate{&first->track(), &second->track()};
        std::size_t given = 0;
        bool refused = false;
        for (std::size_t i = 0; i < 10'000; ++i) {
            const Track& track = *alternate.at(i % 2);
            if (!engine->load(0, track)) {
                refused = true;
                break;
            }
            ledger.loaded(track);
            ++given;
            render_block(*engine);
        }
        REQUIRE(refused);
        REQUIRE(given > ks::engine::kHandoverCapacity);
        const Track* const last = alternate.at((given - 1) % 2);

        // Collecting lets everything proceed.
        for (std::size_t round = 0; round < 10'000 && ledger.outstanding() > 1; ++round) {
            REQUIRE(ledger.collect_all(*engine));
            render_block(*engine);
        }
        REQUIRE(ledger.collect_all(*engine));
        REQUIRE(ledger.outstanding() == 1);
        REQUIRE(engine->mixer().deck(0).track() == last);

        REQUIRE(engine->unload(0));
        render_block(*engine);
        REQUIRE(ledger.collect_all(*engine));
        REQUIRE(ledger.outstanding() == 0);
        REQUIRE(ledger.track_releases() == given);
    }
}

TEST_CASE("Engine: clients attach and detach", "[commands][engine]") {
    using enum CommandType;
    auto engine = std::make_unique<Engine>();
    std::array<Client, ks::engine::kMaxClients + 1> clients;
    Ledger ledger;
    for (std::size_t i = 0; i < ks::engine::kMaxClients; ++i) {
        REQUIRE(engine->attach(clients.at(i).ring()) == std::optional<std::size_t>{i});
    }
    REQUIRE_FALSE(engine->attach(clients.back().ring()).has_value());
    REQUIRE_FALSE(engine->detach(ks::engine::kMaxClients));

    // An attached ring's commands apply from the next block.
    REQUIRE(clients.at(3).send(global_command(SetCrossfader, 0.2)));
    REQUIRE(clients.at(5).send(global_command(SetCueMix, 0.1)));
    render_block(*engine);
    REQUIRE(engine->mixer().settings().crossfader == 0.2);
    REQUIRE(engine->client_stats(3).commands_read == 1);
    REQUIRE(engine->client_stats(5).commands_read == 1);

    // A detached ring is released, and isn't read any more.
    REQUIRE(engine->detach(3));
    ledger.detached(clients.at(3).ring());
    REQUIRE_FALSE(engine->detach(3));
    render_block(*engine);
    REQUIRE(ledger.collect_all(*engine));
    REQUIRE(ledger.rings_released() == std::vector<ks::ipc::CommandRing*>{&clients.at(3).ring()});
    REQUIRE_FALSE(engine->client_stats(3).attached);
    REQUIRE(clients.at(3).send(global_command(SetCrossfader, 0.9)));
    render_block(*engine);
    REQUIRE(engine->mixer().settings().crossfader == 0.2);

    // A slot is reused only once its ring's release has been collected.
    REQUIRE(engine->detach(5));
    ledger.detached(clients.at(5).ring());
    render_block(*engine);
    REQUIRE(engine->attach(clients.back().ring()) == std::optional<std::size_t>{3});
    Client extra;
    REQUIRE_FALSE(engine->attach(extra.ring()).has_value());
    REQUIRE(ledger.collect_all(*engine));
    REQUIRE(ledger.rings_released().size() == 2);
    REQUIRE(ledger.rings_released().back() == &clients.at(5).ring());
    REQUIRE(engine->attach(extra.ring()) == std::optional<std::size_t>{5});

    // Statistics start again for the new client.
    REQUIRE(extra.send(global_command(SetCueMix, 0.6)));
    render_block(*engine);
    REQUIRE(engine->client_stats(5) == ClientStats{.attached = true,
                                                   .corrupt = false,
                                                   .commands_read = 1,
                                                   .last_sequence = 1,
                                                   .rejected = 0,
                                                   .last_rejection = std::nullopt});
    REQUIRE(engine->mixer().settings().cue_mix == 0.6);
    REQUIRE(ledger.detaches_outstanding() == 0);
}

TEST_CASE("Engine: output doesn't depend on block size", "[commands][engine]") {
    using enum CommandType;
    const auto first = music_track(0);
    const auto second = music_track(7'777);
    const auto third = music_track(3'000);

    enum class Kind : std::uint8_t { Load, Unload, Send };
    struct Event {
        std::size_t frame;
        Kind kind;
        std::size_t deck;
        const Track* track;
        Command command;
    };
    const auto send = [](std::size_t frame, const Command& command) {
        return Event{
            .frame = frame, .kind = Kind::Send, .deck = 0, .track = nullptr, .command = command};
    };
    const std::vector<Event> script{
        {.frame = 0, .kind = Kind::Load, .deck = 0, .track = &first->track(), .command = {}},
        {.frame = 0, .kind = Kind::Load, .deck = 1, .track = &second->track(), .command = {}},
        send(0, assign(0, CrossfaderSide::A)),
        send(0, assign(1, CrossfaderSide::B)),
        send(0, global_command(SetCrossfader, 0.0)),
        send(0, playing(0, true)),
        send(0, playing(1, true)),
        send(4'800, global_command(SetCrossfader, 0.25)),
        send(9'600, global_command(SetCrossfader, 0.5)),
        send(12'000, deck_command(SetFader, 1, 0.6)),
        send(14'400, global_command(SetCrossfader, 0.75)),
        send(16'000, deck_command(SetRate, 1, 1.06)),
        send(19'200, global_command(SetCrossfader, 1.0)),
        send(20'000, channel_cue(1, true)),
        send(22'000, deck_command(Seek, 0, 10'000.0)),
        send(24'000, deck_command(SetCuePoint, 1, 500.0)),
        send(26'000, global_command(SetCrossfaderCurve, 0.05)),
        send(26'500, global_command(SetCrossfader, 0.0)),
        send(27'000, global_command(SetCrossfader, 1.0)),
        send(27'250, deck_command(SetTrim, 1, -4.0)),
        {.frame = 28'000, .kind = Kind::Load, .deck = 0, .track = &third->track(), .command = {}},
        send(30'000, playing(0, true)),
        {.frame = 34'000, .kind = Kind::Unload, .deck = 1, .track = nullptr, .command = {}},
    };
    constexpr std::size_t kFrames = 40'000;

    const auto run = [&script](std::size_t block_frames) {
        auto engine = std::make_unique<Engine>();
        Client client;
        Ledger ledger;
        REQUIRE(engine->attach(client.ring()).has_value());
        Outputs out(4, std::vector<float>(kFrames, 0.0F));
        std::size_t next = 0;
        for (std::size_t done = 0; done < kFrames;) {
            while (next < script.size() && script.at(next).frame <= done) {
                const Event& event = script.at(next++);
                switch (event.kind) {
                case Kind::Load:
                    REQUIRE(engine->load(event.deck, *event.track));
                    ledger.loaded(*event.track);
                    break;
                case Kind::Unload:
                    REQUIRE(engine->unload(event.deck));
                    break;
                case Kind::Send:
                    REQUIRE(client.send(event.command));
                    break;
                }
            }
            std::size_t n = std::min(block_frames, kFrames - done);
            if (next < script.size()) {
                n = std::min(n, script.at(next).frame - done);
            }
            process_into(*engine, out, done, n);
            done += n;
            REQUIRE(ledger.collect_all(*engine));
        }
        return out;
    };

    const Outputs reference = run(1);
    REQUIRE(any_sound(reference));
    for (const std::size_t block_frames : {std::size_t{37}, std::size_t{128}, std::size_t{512}}) {
        CAPTURE(block_frames);
        REQUIRE(identical(run(block_frames), reference));
    }
}

namespace {

/// The control thread's side of "share nothing unsafely": random loads,
/// unloads, attaches, detaches, commands, and collections, with a ledger of
/// what it gave and got back.
class ControlThread {
public:
    static constexpr std::size_t kClients = 4;

    explicit ControlThread(Engine& engine)
        : engine_{&engine}, tracks_{constant_track(0.05F), constant_track(0.1F), music_track(0)} {}

    /// One random operation. Returns false if a release didn't match.
    bool step() {
        const std::size_t c = random_() % kClients;
        switch (random_() % 6) {
        case 0:
            load();
            return true;
        case 1:
            static_cast<void>(engine_->unload(random_() % kDecks));
            return true;
        case 2:
            attach(c);
            return true;
        case 3:
            detach(c);
            return true;
        case 4:
            send(c);
            return true;
        default:
            return collect();
        }
    }

    /// Collects every release, and frees the clients whose rings came back.
    /// Returns false if a release didn't match.
    bool collect() {
        const bool ok = ledger_.collect_all(*engine_);
        for (; seen_ < ledger_.rings_released().size(); ++seen_) {
            for (std::size_t c = 0; c < kClients; ++c) {
                if (&clients_.at(c).ring() == ledger_.rings_released().at(seen_)) {
                    with_engine_.at(c) = false;
                }
            }
        }
        return ok;
    }

    /// Unloads every deck and detaches every client. Returns false if the
    /// give ring had no room.
    bool give_everything_back() {
        bool ok = true;
        for (std::size_t deck = 0; deck < kDecks; ++deck) {
            ok = engine_->unload(deck) && ok;
        }
        for (std::size_t c = 0; c < kClients; ++c) {
            const std::optional<std::size_t> slot = slot_of_.at(c);
            if (slot.has_value()) {
                ok = engine_->detach(*slot) && ok;
                ledger_.detached(clients_.at(c).ring());
                slot_of_.at(c).reset();
            }
        }
        return ok;
    }

    [[nodiscard]] const Ledger& ledger() const { return ledger_; }

private:
    void load() {
        const Track& track = tracks_.at(random_() % tracks_.size())->track();
        if (engine_->load(random_() % kDecks, track)) {
            ledger_.loaded(track);
        }
    }

    void attach(std::size_t c) {
        if (!with_engine_.at(c)) {
            slot_of_.at(c) = engine_->attach(clients_.at(c).ring());
            with_engine_.at(c) = slot_of_.at(c).has_value();
        }
    }

    void detach(std::size_t c) {
        const std::optional<std::size_t> slot = slot_of_.at(c);
        if (slot.has_value() && engine_->detach(*slot)) {
            ledger_.detached(clients_.at(c).ring());
            slot_of_.at(c).reset();
        }
    }

    void send(std::size_t c) {
        if (!slot_of_.at(c).has_value()) {
            return;
        }
        std::array<std::uint8_t, 16> choice{};
        for (std::uint8_t& byte : choice) {
            byte = static_cast<std::uint8_t>(random_() & 0xFFU);
        }
        ks::engine::test::ByteReader reader{choice};
        static_cast<void>(clients_.at(c).send(ks::engine::test::valid_command(reader)));
    }

    Engine* engine_;
    std::array<std::unique_ptr<OwnedTrack>, 3> tracks_;
    std::array<Client, kClients> clients_;
    /// Each client's slot while it's attached.
    std::array<std::optional<std::size_t>, kClients> slot_of_{};
    /// Whether each client's ring is with the engine: attached, or detached
    /// but not yet released.
    std::array<bool, kClients> with_engine_{};
    Ledger ledger_;
    std::size_t seen_ = 0;
    std::mt19937_64 random_{77};
};

} // namespace

TEST_CASE("Engine: the control thread and the audio thread share nothing unsafely",
          "[commands][engine]") {
    auto engine = std::make_unique<Engine>();
    ControlThread control{*engine};

    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> blocks{0};
    std::thread audio{[&engine, &done, &blocks] {
        Outputs out(4, std::vector<float>(kBlock, 0.0F));
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < end) {
            process_into(*engine, out, 0, kBlock);
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_release);
    }};
    bool ok = true;
    while (!done.load(std::memory_order_acquire)) {
        ok = control.step() && ok;
    }
    audio.join();
    REQUIRE(ok);
    REQUIRE(blocks.load() > 1'000);

    // Now on this thread alone: give everything back, and let the engine
    // finish.
    const auto settle = [&engine, &control] {
        bool settled = true;
        for (std::size_t i = 0; i < 100; ++i) {
            settled = control.collect() && settled;
            render_block(*engine);
        }
        return control.collect() && settled;
    };
    REQUIRE(settle());
    REQUIRE(control.give_everything_back());
    REQUIRE(settle());

    const Ledger& ledger = control.ledger();
    REQUIRE(ledger.loads() > 100);
    REQUIRE(ledger.rings_released().size() > 10);
    REQUIRE(ledger.outstanding() == 0);
    REQUIRE(ledger.detaches_outstanding() == 0);
    REQUIRE(ledger.track_releases() == ledger.loads());
}
