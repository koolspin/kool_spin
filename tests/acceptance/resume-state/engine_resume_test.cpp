// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/resume-state/spec.md: the engine writes a
// snapshot after every block, and a new engine built on the same state block
// resumes from it (requirements 11 to 17).

#include <ks/engine/block.h>
#include <ks/engine/command.h>
#include <ks/engine/deck.h>
#include <ks/engine/engine.h>
#include <ks/engine/mixer.h>
#include <ks/engine/render_core.h>
#include <ks/engine/resume.h>
#include <ks/engine/track.h>
#include <ks/ipc/command.h>
#include <ks/ipc/state.h>

#include "../commands/test_support.h"
#include "snapshot_support.h"
#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using ks::engine::BusMap;
using ks::engine::CrossfaderSide;
using ks::engine::Deck;
using ks::engine::MixerSettings;
using ks::engine::RenderResult;
using ks::engine::ResumeOutcome;
using ks::engine::Snapshot;
using ks::engine::Track;
using ks::engine::resume_test::constant;
using ks::engine::resume_test::copy_of;
using ks::engine::resume_test::expected_snapshot;
using ks::engine::resume_test::fresh_state;
using ks::engine::resume_test::kBlock;
using ks::engine::resume_test::kRate;
using ks::engine::resume_test::kTrackRate;
using ks::engine::resume_test::music;
using ks::engine::resume_test::Rig;
using ks::engine::resume_test::slot_generation;
using ks::engine::resume_test::slot_snapshot;
using ks::engine::test::Client;
using ks::engine::test::deck_command;
using ks::engine::test::global_command;
using ks::engine::test::playing;
using ks::ipc::CommandType;
using ks::ipc::ResumeState;

constexpr double kInfinity = std::numeric_limits<double>::infinity();

/// Sends `command`, which must fit.
void send(Client& client, const ks::engine::Command& command) {
    REQUIRE(client.send(command));
}

/// Checks the snapshot in slot `generation % 2` against what the rig's engine
/// holds, and that the other slot holds the one before it.
void require_snapshot(Rig& rig, std::uint64_t generation) {
    const Snapshot expected = expected_snapshot(rig.engine(), generation, rig.next_time());
    const std::size_t slot = generation % 2;
    const std::optional<ResumeState> words = ks::engine::resume_test::read_slot(rig.state(), slot);
    REQUIRE(words.has_value());
    const ResumeState actual = words.value_or(ResumeState{});
    const ResumeState wanted = ks::engine::resume_test::expected_words(expected);
    INFO("generation " << generation << ": "
                       << ks::engine::resume_test::word_differences(actual, wanted));
    REQUIRE(ks::engine::resume_test::same_words(actual, wanted));
    REQUIRE(ks::engine::validate(actual, slot) == std::optional<Snapshot>{expected});
    if (generation == 1) {
        REQUIRE_FALSE(slot_snapshot(rig.state(), 1 - slot).has_value());
    } else {
        REQUIRE(slot_generation(rig.state(), 1 - slot) == generation - 1);
    }
}

/// The largest difference between two renders from frame `from` on.
double largest_difference(const ks::engine::test::Outputs& a, const ks::engine::test::Outputs& b,
                          std::size_t from) {
    double largest = 0.0;
    for (std::size_t c = 0; c < a.size(); ++c) {
        for (std::size_t f = from; f < a.at(c).size(); ++f) {
            largest = std::max(largest, std::abs(static_cast<double>(a.at(c).at(f)) -
                                                 static_cast<double>(b.at(c).at(f))));
        }
    }
    return largest;
}

/// The loudest sample in a render.
double peak(const ks::engine::test::Outputs& out) {
    double largest = 0.0;
    for (const std::vector<float>& channel : out) {
        for (const float sample : channel) {
            largest = std::max(largest, std::abs(static_cast<double>(sample)));
        }
    }
    return largest;
}

} // namespace

TEST_CASE("Resume: the engine writes a snapshot after every block", "[resume][engine]") {
    const auto music_track = music(11, kTrackRate);
    const auto quiet_track = constant(12, 0.25F);
    // A track without an ID can't be resumed, so it's written as no track.
    const auto anonymous_track = constant(0, 0.1F);
    Rig a{fresh_state()};
    Client client;
    REQUIRE(a.engine().attach(client.ring()).has_value());

    // Controls, applied before the block of the same number.
    const std::vector<std::pair<std::uint64_t, std::function<void()>>> script{
        {0, [&] { REQUIRE(a.engine().load(0, music_track->track())); }},
        {0, [&] { REQUIRE(a.engine().load(1, quiet_track->track())); }},
        {0, [&] { REQUIRE(a.engine().load(2, anonymous_track->track())); }},
        {1, [&] { send(client, playing(0, true)); }},
        {3, [&] { send(client, deck_command(CommandType::SetRate, 0, 1.05)); }},
        {5, [&] { send(client, deck_command(CommandType::Seek, 1, 1'000.5)); }},
        {6, [&] { send(client, deck_command(CommandType::SetCuePoint, 1, 777.25)); }},
        {7, [&] { send(client, playing(1, true)); }},
        {8, [&] { send(client, deck_command(CommandType::SetRate, 3, -0.7)); }},
        {9, [&] { send(client, playing(2, true)); }},
        {10, [&] { send(client, deck_command(CommandType::SetTrim, 2, -kInfinity)); }},
        {11, [&] { send(client, deck_command(CommandType::SetFader, 1, 0.3)); }},
        {12, [&] { send(client, ks::engine::test::assign(0, CrossfaderSide::A)); }},
        {13, [&] { send(client, ks::engine::test::channel_cue(3, true)); }},
        {14, [&] { send(client, global_command(CommandType::SetCrossfader, 0.2)); }},
        {15, [&] { send(client, global_command(CommandType::SetCrossfaderCurve, 0.05)); }},
        {16, [&] { send(client, global_command(CommandType::SetMainLevel, -3.0)); }},
        {17, [&] { send(client, global_command(CommandType::SetHeadphoneLevel, -kInfinity)); }},
        {18, [&] { send(client, global_command(CommandType::SetCueMix, 0.6)); }},
        {19, [&] { send(client, ks::engine::test::bus_map(BusMap{.main = 4, .cue = 0})); }},
        {20, [&] { REQUIRE(a.engine().unload(1)); }},
        {22, [&] { send(client, deck_command(CommandType::Seek, 0, 5'000.0)); }},
        {25, [&] { send(client, playing(0, false)); }},
        {27, [&] { send(client, deck_command(CommandType::SetCuePoint, 0, 123.5)); }},
    };

    for (std::uint64_t block = 0; block < 40; ++block) {
        for (const auto& [when, control] : script) {
            if (when == block) {
                control();
            }
        }
        REQUIRE(a.render() == RenderResult::Rendered);
        // Generations start at 1 in a zero-filled block.
        require_snapshot(a, block + 1);
    }
}

TEST_CASE("Resume: generations continue across engines", "[resume][engine]") {
    const auto track = music(21);
    const std::array<const Track*, 1> tracks{track->get()};
    Rig a{fresh_state()};
    REQUIRE(a.engine().load(0, track->track()));
    a.render_blocks(5);
    // Generation 5 is in slot 1, and 4 in slot 0.
    REQUIRE(slot_generation(a.state(), 1) == 5);
    REQUIRE(slot_generation(a.state(), 0) == 4);

    SECTION("an engine that starts clean") {
        Rig b{copy_of(a.state())};
        b.render();
        REQUIRE(slot_generation(b.state(), 0) == 6);
        REQUIRE(slot_generation(b.state(), 1) == 5);
        b.render();
        REQUIRE(slot_generation(b.state(), 1) == 7);
    }

    SECTION("an engine that resumes") {
        Rig b{copy_of(a.state()), tracks};
        b.render();
        REQUIRE(slot_generation(b.state(), 0) == 6);
        REQUIRE(slot_generation(b.state(), 1) == 5);
    }

    SECTION("a restart of a restart") {
        Rig b{copy_of(a.state())};
        b.render_blocks(3);
        Rig c{copy_of(b.state()), tracks};
        c.render();
        REQUIRE(slot_generation(c.state(), 1) == 9);
        REQUIRE(slot_generation(c.state(), 0) == 8);
    }

    SECTION("the newest slot corrupted") {
        auto state = copy_of(a.state());
        ks::engine::resume_test::flip_bit(*state, 1, 3, 17);
        Rig b{std::move(state)};
        b.render();
        REQUIRE(slot_generation(b.state(), 1) == 5);
        REQUIRE(slot_generation(b.state(), 0) == 4);
    }

    SECTION("no valid snapshot") {
        auto state = copy_of(a.state());
        ks::engine::resume_test::flip_bit(*state, 0, 3, 17);
        ks::engine::resume_test::flip_bit(*state, 1, 3, 17);
        Rig b{std::move(state), tracks};
        b.render();
        REQUIRE(slot_generation(b.state(), 1) == 1);
        // Slot 0 is still the corrupted one.
        REQUIRE_FALSE(slot_snapshot(b.state(), 0).has_value());
        b.render();
        REQUIRE(slot_generation(b.state(), 0) == 2);
    }
}

TEST_CASE("Resume: decks carry on where they would have been", "[resume][engine]") {
    const auto track_a = music(101, kRate);
    const auto track_b = music(102, kTrackRate);
    const auto track_c = music(103, kRate, 480'000, 7);
    const auto track_d = music(104, kRate, 480'000, 13);
    const std::array<const Track*, 4> tracks{track_a->get(), track_b->get(), track_c->get(),
                                             track_d->get()};
    Rig a{fresh_state()};
    Client client;
    REQUIRE(a.engine().attach(client.ring()).has_value());
    for (std::size_t i = 0; i < 4; ++i) {
        REQUIRE(a.engine().load(i, *tracks.at(i)));
    }
    // Deck 0 at rate 1, deck 1 beatmatched at 1.0159, deck 2 in reverse, and
    // deck 3 paused with a cue point.
    send(client, playing(0, true));
    send(client, deck_command(CommandType::SetRate, 1, 1.0159));
    send(client, playing(1, true));
    send(client, deck_command(CommandType::Seek, 2, 40'000.0));
    send(client, deck_command(CommandType::SetRate, 2, -0.5));
    send(client, playing(2, true));
    send(client, deck_command(CommandType::Seek, 3, 5'000.0));
    send(client, deck_command(CommandType::SetCuePoint, 3, 2'500.0));
    send(client, deck_command(CommandType::SetRate, 3, 0.97));
    // Every mixer control.
    const std::array<double, 4> trims{-kInfinity, 6.0, -3.5, 12.0};
    const std::array<double, 4> faders{0.3, 0.5, 0.7, 0.9};
    const std::array<CrossfaderSide, 4> sides{CrossfaderSide::A, CrossfaderSide::B,
                                              CrossfaderSide::Thru, CrossfaderSide::A};
    for (std::size_t i = 0; i < 4; ++i) {
        send(client, deck_command(CommandType::SetTrim, i, trims.at(i)));
        send(client, deck_command(CommandType::SetFader, i, faders.at(i)));
        send(client, ks::engine::test::assign(i, sides.at(i)));
        send(client, ks::engine::test::channel_cue(i, i % 2 == 0));
    }
    a.render();
    send(client, global_command(CommandType::SetCrossfader, 0.25));
    send(client, global_command(CommandType::SetCrossfaderCurve, 0.05));
    send(client, global_command(CommandType::SetMainLevel, -2.0));
    send(client, global_command(CommandType::SetHeadphoneLevel, -kInfinity));
    send(client, global_command(CommandType::SetCueMix, 0.75));
    send(client, ks::engine::test::bus_map(BusMap{.main = 2, .cue = 4}));
    a.render_blocks(29);
    REQUIRE(a.settings().bus_map == BusMap{.main = 2, .cue = 4});

    // B is given a different bus map. Its first block starts `gap` after A's
    // last ended; a negative gap counts as none.
    const auto resume_after = [&](std::chrono::nanoseconds gap, double seconds) {
        Rig b{copy_of(a.state()), tracks, BusMap{.main = 6, .cue = 4}};
        const ResumeOutcome expected_outcome{.generation = a.blocks(), .tracks = tracks};
        REQUIRE(b.engine().resume_outcome() == expected_outcome);
        b.set_next_time(a.next_time() + gap);
        REQUIRE(b.render() == RenderResult::Rendered);

        for (std::size_t i = 0; i < 4; ++i) {
            INFO("deck " << i);
            const Deck& before = a.deck(i);
            const Deck& after = b.deck(i);
            REQUIRE(after.track() == tracks.at(i));
            REQUIRE(after.playing() == before.playing());
            REQUIRE(after.rate() == before.rate());
            REQUIRE(after.cue() == before.cue());
            const auto track_rate = static_cast<double>(tracks.at(i)->sample_rate());
            double expected = before.position();
            if (before.playing()) {
                // The gap's travel, then one block's.
                expected += before.rate() * track_rate * seconds;
                expected += static_cast<double>(kBlock) * before.rate() * track_rate / kRate;
            }
            INFO("position " << after.position() << ", expected " << expected);
            REQUIRE(std::abs(after.position() - expected) <= 1e-4);
        }
        REQUIRE(b.settings() == a.settings());
    };

    SECTION("0.75 s later") {
        resume_after(750ms, 0.75);
    }

    SECTION("a first block that starts before the snapshot's host time") {
        resume_after(-1s, 0.0);
    }
}

TEST_CASE("Resume: with no gap, the output carries on", "[resume][engine]") {
    const auto track_a = music(201, kRate);
    const auto track_b = music(202, kTrackRate, 480'000, 1'000);
    const std::array<const Track*, 2> tracks{track_a->get(), track_b->get()};
    Rig a{fresh_state()};
    Client client;
    REQUIRE(a.engine().attach(client.ring()).has_value());
    REQUIRE(a.engine().load(0, track_a->track()));
    REQUIRE(a.engine().load(1, track_b->track()));
    send(client, playing(0, true));
    send(client, playing(1, true));
    send(client, deck_command(CommandType::SetRate, 1, 0.98));
    send(client, ks::engine::test::assign(0, CrossfaderSide::A));
    send(client, ks::engine::test::assign(1, CrossfaderSide::B));
    send(client, global_command(CommandType::SetCrossfader, 0.4));
    send(client, deck_command(CommandType::SetFader, 0, 0.5));
    send(client, deck_command(CommandType::SetFader, 1, 0.5));
    a.render_blocks(50);

    Rig b{copy_of(a.state()), tracks};
    b.set_next_time(a.next_time());
    const auto from_a = a.render_frames(7'200);
    const auto from_b = b.render_frames(7'200);

    // From the end of the fade, 50 ms in.
    REQUIRE(peak(from_a) > 0.01);
    REQUIRE(largest_difference(from_a, from_b, 2'400) <= 1e-5);
}

TEST_CASE("Resume: playback fades in over 50 ms", "[resume][engine]") {
    for (const auto& [sample_rate, fade_frames] :
         std::array<std::pair<std::uint32_t, std::size_t>, 2>{{{48'000, 2'400}, {44'100, 2'205}}}) {
        INFO("device rate " << sample_rate);
        const auto track = constant(301, 0.5F);
        const std::array<const Track*, 1> tracks{track->get()};
        Rig a{fresh_state(), BusMap{}, sample_rate};
        Client client;
        REQUIRE(a.engine().attach(client.ring()).has_value());
        REQUIRE(a.engine().load(0, track->track()));
        send(client, deck_command(CommandType::Seek, 0, 100'000.0));
        send(client, playing(0, true));
        a.render_blocks(20);

        Rig b{copy_of(a.state()), tracks, BusMap{}, sample_rate};
        b.set_next_time(a.next_time());
        const auto out = b.render_frames(4'096);

        for (const std::vector<float>& channel : out) {
            REQUIRE(channel.at(0) == 0.0F);
        }
        std::size_t too_loud = 0;
        for (std::size_t f = 0; f < fade_frames; ++f) {
            const double bound =
                (0.5 * static_cast<double>(f) / static_cast<double>(fade_frames)) + 1e-3;
            for (const std::vector<float>& channel : out) {
                if (std::abs(static_cast<double>(channel.at(f))) > bound) {
                    ++too_loud;
                }
            }
        }
        REQUIRE(too_loud == 0);
        std::size_t off = 0;
        for (std::size_t f = fade_frames; f < out.at(0).size(); ++f) {
            if (std::abs(static_cast<double>(out.at(0).at(f)) - 0.5) > 1e-3 ||
                std::abs(static_cast<double>(out.at(1).at(f)) + 0.5) > 1e-3) {
                ++off;
            }
        }
        REQUIRE(off == 0);
    }
}

TEST_CASE("Resume: decks without their track come back empty", "[resume][engine]") {
    std::array<std::unique_ptr<ks::engine::resume_test::TestTrack>, 4> originals{
        constant(1, 0.1F), constant(2, 0.1F), constant(3, 0.1F), constant(4, 0.1F)};
    const auto anonymous = constant(0, 0.1F);
    const auto unrelated = constant(99, 0.1F);
    const auto first_four = constant(4, 0.1F);
    const auto second_four = constant(4, 0.1F);
    Rig a{fresh_state()};
    Client client;
    REQUIRE(a.engine().attach(client.ring()).has_value());
    const std::array<double, 4> rates{1.0, 0.9, 1.1, 1.2};
    for (std::size_t i = 0; i < 4; ++i) {
        REQUIRE(a.engine().load(i, originals.at(i)->track()));
        send(client, deck_command(CommandType::SetRate, i, rates.at(i)));
        send(client, playing(i, true));
    }
    a.render_blocks(10);

    // Not tracks 1 and 3; two tracks with ID 4, of which the first is taken.
    const std::array<const Track*, 6> given{
        nullptr,          anonymous->get(),  originals.at(1)->get(),
        unrelated->get(), first_four->get(), second_four->get()};
    Rig b{copy_of(a.state()), given};
    const ResumeOutcome expected_outcome{
        .generation = a.blocks(),
        .tracks = {nullptr, originals.at(1)->get(), nullptr, first_four->get()}};
    REQUIRE(b.engine().resume_outcome() == expected_outcome);
    REQUIRE_FALSE(b.engine().collect().has_value());

    b.set_next_time(a.next_time());
    REQUIRE(b.render() == RenderResult::Rendered);
    for (const std::size_t i : {std::size_t{0}, std::size_t{2}}) {
        INFO("deck " << i);
        REQUIRE(b.deck(i).track() == nullptr);
        REQUIRE_FALSE(b.deck(i).playing());
        // Its rate is still restored.
        REQUIRE(b.deck(i).rate() == rates.at(i));
    }
    REQUIRE(b.deck(1).track() == originals.at(1)->get());
    REQUIRE(b.deck(1).playing());
    REQUIRE(b.deck(3).track() == first_four->get());
    REQUIRE(b.deck(3).playing());
    REQUIRE_FALSE(b.engine().collect().has_value());
}

TEST_CASE("Resume: decks stop at the ends", "[resume][engine]") {
    const auto forward = constant(401, 0.2F, 100'000);
    const auto reverse = constant(402, 0.2F, 100'000);
    const std::array<const Track*, 2> tracks{forward->get(), reverse->get()};
    Rig a{fresh_state()};
    Client client;
    REQUIRE(a.engine().attach(client.ring()).has_value());
    REQUIRE(a.engine().load(0, forward->track()));
    REQUIRE(a.engine().load(1, reverse->track()));
    // About 1,000 frames from the end, and from the start in reverse.
    send(client, deck_command(CommandType::Seek, 0, 99'000.0));
    send(client, playing(0, true));
    send(client, deck_command(CommandType::Seek, 1, 1'000.0));
    send(client, deck_command(CommandType::SetRate, 1, -1.0));
    send(client, playing(1, true));
    a.render();

    Rig b{copy_of(a.state()), tracks};
    b.set_next_time(a.next_time() + 1s);
    REQUIRE(b.render() == RenderResult::Rendered);
    // Each stops at its end and pauses, as a deck that reaches an end does
    // (specs/deck/spec.md requirement 17).
    REQUIRE(b.deck(0).position() == 100'000.0);
    REQUIRE_FALSE(b.deck(0).playing());
    REQUIRE(b.deck(1).position() == 0.0);
    REQUIRE_FALSE(b.deck(1).playing());
}

TEST_CASE("Resume: a corrupted resume state starts clean", "[resume][engine]") {
    const auto track = music(1);
    const std::array<const Track*, 1> tracks{track->get()};
    Rig a{fresh_state()};
    Client client;
    REQUIRE(a.engine().attach(client.ring()).has_value());
    REQUIRE(a.engine().load(0, track->track()));
    send(client, playing(0, true));
    send(client, deck_command(CommandType::SetFader, 0, 0.5));
    send(client, ks::engine::test::bus_map(BusMap{.main = 2, .cue = 4}));
    a.render_blocks(20);
    // Generation 20 is in slot 0, and 19 in slot 1.

    const BusMap map{.main = 4, .cue = 6};
    MixerSettings clean_settings;
    clean_settings.bus_map = map;

    using ks::engine::resume_test::forge;
    const std::vector<std::pair<std::string, std::function<void(ks::ipc::EngineState&)>>>
        corruptions{
            {"zero-filled", [](ks::ipc::EngineState& s) { s = ks::ipc::EngineState{}; }},
            {"bytes 0xFF",
             [](ks::ipc::EngineState& s) {
                 for (auto& lock : s.resume) {
                     lock.words.fill(~std::uint64_t{0});
                 }
             }},
            {"bytes from a fixed seed",
             [](ks::ipc::EngineState& s) {
                 std::mt19937_64 random{7};
                 for (auto& lock : s.resume) {
                     for (std::uint64_t& word : lock.words) {
                         word = random();
                     }
                 }
             }},
            {"a flipped bit",
             [](ks::ipc::EngineState& s) {
                 ks::engine::resume_test::flip_bit(s, 0, 9, 33);
                 ks::engine::resume_test::flip_bit(s, 1, 9, 33);
             }},
            {"odd sequence numbers",
             [](ks::ipc::EngineState& s) {
                 for (auto& lock : s.resume) {
                     lock.sequence += 1;
                 }
             }},
            {"a NaN position",
             [](ks::ipc::EngineState& s) {
                 forge(s, [](ResumeState& w) {
                     w.decks.at(0).position =
                         ks::engine::resume_test::bits(std::numeric_limits<double>::quiet_NaN());
                 });
             }},
            {"rate 10",
             [](ks::ipc::EngineState& s) {
                 forge(s, [](ResumeState& w) {
                     w.decks.at(0).rate = ks::engine::resume_test::bits(10.0);
                 });
             }},
            {"fader 2",
             [](ks::ipc::EngineState& s) {
                 forge(s, [](ResumeState& w) {
                     w.channels.at(0).fader = ks::engine::resume_test::bits(2.0);
                 });
             }},
            {"bus map 1 and 3",
             [](ks::ipc::EngineState& s) {
                 forge(s, [](ResumeState& w) {
                     w.bus_main = 1;
                     w.bus_cue = 3;
                 });
             }},
        };

    for (const auto& [name, corrupt] : corruptions) {
        DYNAMIC_SECTION(name) {
            auto state = copy_of(a.state());
            corrupt(*state);
            Rig b{std::move(state), tracks, map};
            REQUIRE(b.engine().resume_outcome() == ResumeOutcome{});
            REQUIRE_FALSE(b.engine().collect().has_value());
            for (std::size_t i = 0; i < 4; ++i) {
                REQUIRE(b.deck(i).track() == nullptr);
                REQUIRE_FALSE(b.deck(i).playing());
            }
            REQUIRE(b.settings() == clean_settings);

            b.set_next_time(a.next_time());
            REQUIRE(b.render() == RenderResult::Rendered);
            // It writes generation 1 first.
            REQUIRE(slot_snapshot(b.state(), 1) ==
                    std::optional<Snapshot>{expected_snapshot(b.engine(), 1, b.next_time())});

            const auto out = b.render_frames(99 * kBlock);
            std::size_t not_silent = 0;
            for (const std::vector<float>& channel : out) {
                not_silent += static_cast<std::size_t>(
                    std::ranges::count_if(channel, [](float s) { return s != 0.0F; }));
            }
            REQUIRE(not_silent == 0);
            REQUIRE_FALSE(b.engine().collect().has_value());
        }
    }

    SECTION("only the newest slot corrupted: the older one") {
        auto state = copy_of(a.state());
        ks::engine::resume_test::flip_bit(*state, 0, 9, 33);
        Rig b{std::move(state), tracks, map};
        const ResumeOutcome expected_outcome{.generation = 19, .tracks = {track->get()}};
        REQUIRE(b.engine().resume_outcome() == expected_outcome);
        REQUIRE(b.settings().bus_map == BusMap{.main = 2, .cue = 4});
    }
}

TEST_CASE("Resume: references come back through collect()", "[resume][engine]") {
    const auto track = constant(7, 0.2F);
    const std::array<const Track*, 1> tracks{track->get()};
    Rig a{fresh_state()};
    Client client_a;
    REQUIRE(a.engine().attach(client_a.ring()).has_value());
    REQUIRE(a.engine().load(0, track->track()));
    REQUIRE(a.engine().load(1, track->track()));
    send(client_a, playing(0, true));
    send(client_a, playing(1, true));
    a.render_blocks(5);

    Rig b{copy_of(a.state()), tracks};
    const ResumeOutcome expected_outcome{.generation = a.blocks(),
                                         .tracks = {track->get(), track->get(), nullptr, nullptr}};
    REQUIRE(b.engine().resume_outcome() == expected_outcome);
    ks::engine::test::Ledger ledger;
    ledger.loaded(track->track());
    ledger.loaded(track->track());

    b.set_next_time(a.next_time());
    b.render();
    REQUIRE(b.engine().unload(0));
    REQUIRE(b.engine().unload(1));
    b.render_blocks(10);
    REQUIRE(ledger.collect_all(b.engine()));
    REQUIRE(ledger.track_releases() == 2);
    REQUIRE(ledger.outstanding() == 0);

    // Loads, unloads, and commands work as usual afterwards.
    Client client_b;
    REQUIRE(b.engine().attach(client_b.ring()).has_value());
    REQUIRE(b.engine().load(2, track->track()));
    ledger.loaded(track->track());
    send(client_b, playing(2, true));
    b.render_blocks(2);
    REQUIRE(b.deck(2).track() == track->get());
    REQUIRE(b.deck(2).playing());
    REQUIRE(b.engine().unload(2));
    b.render_blocks(10);
    REQUIRE(ledger.collect_all(b.engine()));
    REQUIRE(ledger.track_releases() == 3);
    REQUIRE(ledger.outstanding() == 0);
}

TEST_CASE("Resume: output doesn't depend on block size", "[resume][engine]") {
    const auto track_a = music(501, kRate);
    const auto track_b = music(502, kTrackRate, 480'000, 300);
    const std::array<const Track*, 2> tracks{track_a->get(), track_b->get()};
    Rig a{fresh_state()};
    Client client;
    REQUIRE(a.engine().attach(client.ring()).has_value());
    REQUIRE(a.engine().load(0, track_a->track()));
    REQUIRE(a.engine().load(1, track_b->track()));
    send(client, playing(0, true));
    send(client, playing(1, true));
    send(client, deck_command(CommandType::SetRate, 1, 1.03));
    send(client, ks::engine::test::assign(0, CrossfaderSide::A));
    send(client, ks::engine::test::assign(1, CrossfaderSide::B));
    send(client, deck_command(CommandType::SetFader, 0, 0.5));
    send(client, deck_command(CommandType::SetFader, 1, 0.5));
    a.render_blocks(10);

    static constexpr std::array<std::size_t, 9> kSizes{1, 7, 64, 512, 100, 33, 255, 2, 300};
    Rig steady{copy_of(a.state()), tracks};
    Rig varied{copy_of(a.state()), tracks};
    Client steady_client;
    Client varied_client;
    REQUIRE(steady.engine().attach(steady_client.ring()).has_value());
    REQUIRE(varied.engine().attach(varied_client.ring()).has_value());
    steady.set_next_time(a.next_time() + 300ms);
    varied.set_next_time(a.next_time() + 300ms);

    auto from_steady = steady.render_frames(4'800);
    auto from_varied = varied.render_frames(4'800, kSizes);
    // The same commands, before the same frame.
    for (Client* const c : {&steady_client, &varied_client}) {
        send(*c, deck_command(CommandType::SetRate, 0, 1.1));
        send(*c, global_command(CommandType::SetCrossfader, 0.7));
        send(*c, deck_command(CommandType::Seek, 1, 20'000.0));
    }
    const auto more_steady = steady.render_frames(4'800);
    const auto more_varied = varied.render_frames(4'800, kSizes);
    for (std::size_t c = 0; c < from_steady.size(); ++c) {
        from_steady.at(c).insert(from_steady.at(c).end(), more_steady.at(c).begin(),
                                 more_steady.at(c).end());
        from_varied.at(c).insert(from_varied.at(c).end(), more_varied.at(c).begin(),
                                 more_varied.at(c).end());
    }

    REQUIRE(peak(from_steady) > 0.01);
    REQUIRE(from_steady == from_varied);
}
