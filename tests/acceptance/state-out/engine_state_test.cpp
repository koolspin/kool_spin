// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/state-out/spec.md: the engine publishes deck,
// mixer, and status blocks after every block the render core renders
// (requirements 3 to 6 and 16 to 18).

#include <ks/engine/block.h>
#include <ks/engine/command.h>
#include <ks/engine/engine.h>
#include <ks/engine/mixer.h>
#include <ks/engine/render_core.h>
#include <ks/ipc/command.h>
#include <ks/ipc/layout.h>
#include <ks/ipc/state.h>

#include "../commands/test_support.h"
#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using ks::engine::BusMap;
using ks::engine::CrossfaderSide;
using ks::engine::DriverId;
using ks::engine::kDecks;
using ks::engine::kDefaultDriver;
using ks::engine::OutputState;
using ks::engine::RenderResult;
using ks::engine::state_test::all_zero;
using ks::engine::state_test::bits;
using ks::engine::state_test::EngineRig;
using ks::engine::state_test::IdTrack;
using ks::engine::state_test::kBlock;
using ks::engine::state_test::kRate;
using ks::engine::state_test::read;
using ks::engine::state_test::real;
using ks::engine::state_test::sequence;
using ks::engine::state_test::time_word;
using ks::engine::test::assign;
using ks::engine::test::bus_map;
using ks::engine::test::channel_cue;
using ks::engine::test::Client;
using ks::engine::test::deck_command;
using ks::engine::test::encode;
using ks::engine::test::global_command;
using ks::engine::test::playing;
using ks::ipc::CommandType;
using std::chrono::milliseconds;

constexpr double kInfinity = std::numeric_limits<double>::infinity();

/// Requires every block to hold what the engine and the render core hold.
void require_published(EngineRig& rig) {
    const std::string diff = ks::engine::state_test::differences(rig);
    CAPTURE(rig.blocks(), diff);
    REQUIRE(diff.empty());
}

/// Every seqlock's sequence number.
std::vector<std::uint64_t> sequences(ks::ipc::EngineState& state) {
    std::vector<std::uint64_t> out;
    out.reserve(kDecks + 2);
    for (auto& deck : state.decks) {
        out.push_back(sequence(deck));
    }
    out.push_back(sequence(state.mixer));
    out.push_back(sequence(state.status));
    return out;
}

} // namespace

TEST_CASE("State: each rendered block publishes everything once", "[state][engine]") {
    EngineRig rig;
    ks::ipc::EngineState& state = rig.state();

    // Nothing is written before the first block.
    REQUIRE(sequences(state) == std::vector<std::uint64_t>(kDecks + 2, 0));
    for (auto& deck : state.decks) {
        REQUIRE(all_zero(read(deck)));
    }
    REQUIRE(all_zero(read(state.mixer)));
    REQUIRE(all_zero(read(state.status)));

    rig.render_blocks(10);
    REQUIRE(sequences(state) == std::vector<std::uint64_t>(kDecks + 2, 20));
    for (auto& deck : state.decks) {
        REQUIRE(read(deck).heartbeat == 10);
    }
    REQUIRE(read(state.mixer).heartbeat == 10);
    REQUIRE(read(state.status).heartbeat == 10);
    require_published(rig);

    // Skipped and rejected blocks change nothing.
    rig.core().activate(DriverId{7});
    REQUIRE(rig.render() == RenderResult::Skipped);
    rig.core().activate(kDefaultDriver);
    REQUIRE(rig.render(0) == RenderResult::Rejected);
    REQUIRE(sequences(state) == std::vector<std::uint64_t>(kDecks + 2, 20));
    REQUIRE(read(state.status).heartbeat == 10);

    REQUIRE(rig.render() == RenderResult::Rendered);
    REQUIRE(sequences(state) == std::vector<std::uint64_t>(kDecks + 2, 22));
    REQUIRE(read(state.status).heartbeat == 11);
    require_published(rig);
}

TEST_CASE("State: decks", "[state][engine]") {
    using enum CommandType;
    EngineRig rig;
    ks::ipc::EngineState& state = rig.state();
    Client client;
    REQUIRE(rig.engine().attach(client.ring()) == std::optional<std::size_t>{0});
    REQUIRE(rig.render() == RenderResult::Rendered);
    require_published(rig);

    // Empty decks.
    for (auto& lock : state.decks) {
        const ks::ipc::DeckState deck = read(lock);
        REQUIRE(deck.flags == 0);
        REQUIRE(deck.track_id == 0);
        REQUIRE(deck.track_sample_rate == 0);
        REQUIRE(deck.track_frames == 0);
        REQUIRE(deck.frames_ready == 0);
    }

    IdTrack first{48'000, 101};
    IdTrack second{96'000, 202, 0.2F, 44'100};
    IdTrack decoding{48'000, 303, 0.3F, kRate, false};
    decoding.track().publish(10'000, false);
    REQUIRE(rig.engine().load(0, first.track()));
    REQUIRE(rig.engine().load(1, second.track()));
    REQUIRE(rig.engine().load(2, decoding.track()));
    REQUIRE(rig.render() == RenderResult::Rendered);
    require_published(rig);

    ks::ipc::DeckState deck = read(state.decks.at(0));
    REQUIRE(deck.track_id == 101);
    REQUIRE(deck.track_sample_rate == kRate);
    REQUIRE(deck.track_frames == 48'000);
    REQUIRE(deck.frames_ready == 48'000);
    REQUIRE(deck.flags == (ks::ipc::kDeckLoaded | ks::ipc::kDeckComplete));
    deck = read(state.decks.at(1));
    REQUIRE(deck.track_id == 202);
    REQUIRE(deck.track_sample_rate == 44'100);
    REQUIRE(deck.track_frames == 96'000);
    deck = read(state.decks.at(2));
    REQUIRE(deck.track_id == 303);
    REQUIRE(deck.frames_ready == 10'000);
    REQUIRE(deck.flags == ks::ipc::kDeckLoaded);
    REQUIRE(read(state.decks.at(3)).flags == 0);

    // Play, seek, rate, and a cue point; deck 2 plays up to its decode limit
    // and waits there.
    REQUIRE(client.send(playing(0, true)));
    REQUIRE(client.send(deck_command(Seek, 0, 1'000.0)));
    REQUIRE(client.send(deck_command(SetCuePoint, 0, 480.0)));
    REQUIRE(client.send(playing(1, true)));
    REQUIRE(client.send(deck_command(SetRate, 1, 1.06)));
    REQUIRE(client.send(playing(2, true)));
    bool waited = false;
    for (std::size_t i = 0; i < 200; ++i) {
        REQUIRE(rig.render() == RenderResult::Rendered);
        require_published(rig);
        waited = waited || (read(state.decks.at(2)).flags & ks::ipc::kDeckWaiting) != 0;
    }
    REQUIRE(waited);
    deck = read(state.decks.at(0));
    REQUIRE((deck.flags & ks::ipc::kDeckPlaying) != 0);
    REQUIRE(deck.cue == bits(480.0));
    REQUIRE(real(deck.position) > 1'000.0);
    REQUIRE(read(state.decks.at(1)).rate == bits(1.06));

    // Decoding finishes.
    decoding.track().publish(48'000, true);
    for (std::size_t i = 0; i < 4; ++i) {
        REQUIRE(rig.render() == RenderResult::Rendered);
        require_published(rig);
    }
    deck = read(state.decks.at(2));
    REQUIRE(deck.frames_ready == 48'000);
    REQUIRE((deck.flags & ks::ipc::kDeckComplete) != 0);
    REQUIRE((deck.flags & ks::ipc::kDeckWaiting) == 0);

    // Unloading empties the main head.
    REQUIRE(rig.engine().unload(0));
    for (std::size_t i = 0; i < 10; ++i) {
        REQUIRE(rig.render() == RenderResult::Rendered);
        require_published(rig);
    }
    deck = read(state.decks.at(0));
    REQUIRE((deck.flags & ks::ipc::kDeckLoaded) == 0);
    REQUIRE(deck.track_id == 0);
    REQUIRE(deck.track_frames == 0);
}

TEST_CASE("State: the visual play position", "[state][engine]") {
    EngineRig rig;
    rig.set_latency(milliseconds{5});
    ks::ipc::EngineState& state = rig.state();
    Client client;
    IdTrack track{480'000, 7};
    REQUIRE(rig.engine().attach(client.ring()) == std::optional<std::size_t>{0});
    REQUIRE(rig.engine().load(0, track.track()));
    REQUIRE(rig.render() == RenderResult::Rendered);
    REQUIRE(client.send(playing(0, true)));

    const auto block_duration = ks::engine::frames_to_duration(kBlock, kRate);
    std::uint64_t last_time = 0;
    for (std::size_t i = 0; i < 100; ++i) {
        CAPTURE(i);
        REQUIRE(rig.render() == RenderResult::Rendered);
        require_published(rig);
        const ks::ipc::DeckState deck = read(state.decks.at(0));
        // The frame at `position` is due at the end of the block, and heard
        // 5 ms later.
        REQUIRE(deck.host_time == time_word(rig.next_time()));
        REQUIRE(deck.output_latency == 5'000'000);
        REQUIRE(real(deck.position) == static_cast<double>(kBlock * (i + 1)));
        if (i > 0) {
            REQUIRE(deck.host_time - last_time ==
                    static_cast<std::uint64_t>(block_duration.count()));
        }
        last_time = deck.host_time;
        // Every deck carries the time, loaded or not.
        REQUIRE(read(state.decks.at(3)).host_time == deck.host_time);
    }
}

TEST_CASE("State: the mixer", "[state][engine]") {
    using enum CommandType;
    EngineRig rig;
    ks::ipc::EngineState& state = rig.state();
    Client client;
    REQUIRE(rig.engine().attach(client.ring()) == std::optional<std::size_t>{0});
    REQUIRE(rig.render() == RenderResult::Rendered);
    require_published(rig);

    // The defaults.
    ks::ipc::MixerState mixer = read(state.mixer);
    for (const ks::ipc::ChannelState& channel : mixer.channels) {
        REQUIRE(channel.trim_db == bits(0.0));
        REQUIRE(channel.fader == bits(1.0));
        REQUIRE(channel.side == 2);
        REQUIRE(channel.cue == 0);
    }
    REQUIRE(mixer.crossfader == bits(0.5));
    REQUIRE(mixer.bus_main == 0);
    REQUIRE(mixer.bus_cue == 2);

    // Every control.
    REQUIRE(client.send(deck_command(SetTrim, 0, -kInfinity)));
    REQUIRE(client.send(deck_command(SetTrim, 1, 6.0)));
    REQUIRE(client.send(deck_command(SetFader, 2, 0.3)));
    REQUIRE(client.send(assign(0, CrossfaderSide::A)));
    REQUIRE(client.send(assign(3, CrossfaderSide::B)));
    REQUIRE(client.send(channel_cue(1, true)));
    REQUIRE(client.send(global_command(SetCrossfader, 0.7)));
    REQUIRE(client.send(global_command(SetCrossfaderCurve, 0.05)));
    REQUIRE(client.send(global_command(SetMainLevel, -kInfinity)));
    REQUIRE(client.send(global_command(SetHeadphoneLevel, -3.0)));
    REQUIRE(client.send(global_command(SetCueMix, 0.4)));
    REQUIRE(client.send(bus_map(BusMap{.main = 4, .cue = 6})));
    REQUIRE(rig.render() == RenderResult::Rendered);
    require_published(rig);

    mixer = read(state.mixer);
    REQUIRE(mixer.heartbeat == 2);
    REQUIRE(mixer.channels.at(0).trim_db == bits(-kInfinity));
    REQUIRE(mixer.channels.at(0).side == 0);
    REQUIRE(mixer.channels.at(1).trim_db == bits(6.0));
    REQUIRE(mixer.channels.at(1).cue == 1);
    REQUIRE(mixer.channels.at(2).fader == bits(0.3));
    REQUIRE(mixer.channels.at(3).side == 1);
    REQUIRE(mixer.crossfader == bits(0.7));
    REQUIRE(mixer.crossfader_transition == bits(0.05));
    REQUIRE(mixer.main_level_db == bits(-kInfinity));
    REQUIRE(mixer.headphone_level_db == bits(-3.0));
    REQUIRE(mixer.cue_mix == bits(0.4));
    REQUIRE(mixer.bus_main == 4);
    REQUIRE(mixer.bus_cue == 6);
}

TEST_CASE("State: status", "[state][engine]") {
    using enum CommandType;
    EngineRig rig;
    rig.set_latency(milliseconds{4});
    ks::ipc::EngineState& state = rig.state();
    Client client;
    IdTrack bad{48'000, 1, std::numeric_limits<float>::quiet_NaN()};
    IdTrack loud{48'000, 2, 0.9F};
    REQUIRE(rig.engine().attach(client.ring()) == std::optional<std::size_t>{0});
    REQUIRE(rig.engine().load(0, bad.track()));
    REQUIRE(rig.engine().load(1, loud.track()));
    REQUIRE(rig.render() == RenderResult::Rendered);
    require_published(rig);

    ks::ipc::EngineStatus status = read(state.status);
    REQUIRE(status.heartbeat == 1);
    REQUIRE(status.frames_rendered == kBlock);
    REQUIRE(status.sample_rate == kRate);
    REQUIRE(status.host_time == time_word(rig.next_time()));
    REQUIRE(status.output_latency == 4'000'000);
    REQUIRE(status.output_state == 0);
    REQUIRE(status.flags == 0);
    REQUIRE(status.render_time > 0);

    // A track with NaN samples.
    REQUIRE(client.send(playing(0, true)));
    for (std::size_t i = 0; i < 5; ++i) {
        REQUIRE(rig.render() == RenderResult::Rendered);
        require_published(rig);
    }
    REQUIRE(read(state.status).non_finite_samples > 0);
    REQUIRE(rig.engine().unload(0));
    rig.render_blocks(10);
    require_published(rig);

    // A loud track with +12 dB trim.
    REQUIRE(client.send(deck_command(SetTrim, 1, 12.0)));
    REQUIRE(client.send(playing(1, true)));
    for (std::size_t i = 0; i < 10; ++i) {
        REQUIRE(rig.render() == RenderResult::Rendered);
        require_published(rig);
    }
    status = read(state.status);
    REQUIRE(status.limited_frames > 0);
    REQUIRE(status.heartbeat == rig.blocks());
    REQUIRE(status.frames_rendered == kBlock * rig.blocks());
    REQUIRE(real(status.render_load) > 0.0);
    REQUIRE(real(status.peak_render_load) >= real(status.render_load));

    // Muting, then silence once the fade ends.
    rig.core().set_muted(true);
    REQUIRE(rig.render() == RenderResult::Rendered);
    require_published(rig);
    REQUIRE((read(state.status).flags & ks::ipc::kStatusMuted) != 0);
    for (std::size_t i = 0;
         i < 20 && read(state.status).flags != (ks::ipc::kStatusMuted | ks::ipc::kStatusSilenced);
         ++i) {
        REQUIRE(rig.render() == RenderResult::Rendered);
        require_published(rig);
    }
    REQUIRE(read(state.status).flags == (ks::ipc::kStatusMuted | ks::ipc::kStatusSilenced));
    rig.core().set_muted(false);
    REQUIRE(rig.render() == RenderResult::Rendered);
    require_published(rig);
    REQUIRE(read(state.status).flags == 0);

    // The output state, from the next block.
    rig.core().set_output_state(OutputState::Freewheeling);
    REQUIRE(rig.render() == RenderResult::Rendered);
    require_published(rig);
    REQUIRE(read(state.status).output_state == 2);
    rig.core().set_output_state(OutputState::Attaching);
    REQUIRE(rig.render() == RenderResult::Rendered);
    require_published(rig);
    REQUIRE(read(state.status).output_state == 3);
}

TEST_CASE("State: acknowledgements", "[state][engine]") {
    using enum CommandType;
    EngineRig rig;
    ks::ipc::EngineState& state = rig.state();
    Client valid;
    Client invalid;
    Client mixed;
    REQUIRE(rig.engine().attach(valid.ring()) == std::optional<std::size_t>{0});
    REQUIRE(rig.engine().attach(invalid.ring()) == std::optional<std::size_t>{1});
    REQUIRE(rig.engine().attach(mixed.ring()) == std::optional<std::size_t>{2});
    REQUIRE(rig.render() == RenderResult::Rendered);
    require_published(rig);

    ks::ipc::EngineStatus status = read(state.status);
    for (std::size_t i = 0; i < 3; ++i) {
        REQUIRE(status.clients.at(i).flags == ks::ipc::kClientAttached);
        REQUIRE(status.clients.at(i).commands_read == 0);
    }
    for (std::size_t i = 3; i < ks::ipc::kStateClients; ++i) {
        REQUIRE(status.clients.at(i).flags == 0);
    }

    for (std::size_t i = 0; i < 3; ++i) {
        REQUIRE(valid.send(deck_command(SetFader, 0, 0.5)));
    }
    ks::ipc::CommandSlot unknown = encode(1, deck_command(SetFader, 0, 0.5));
    unknown.type = 99;
    ks::ipc::CommandSlot bad_target = encode(2, deck_command(SetFader, 0, 0.5));
    bad_target.target = 9;
    REQUIRE(invalid.send_slot(unknown));
    REQUIRE(invalid.send_slot(bad_target));
    REQUIRE(mixed.send(global_command(SetCrossfader, 0.25)));
    REQUIRE(mixed.send(global_command(SetCrossfader, std::numeric_limits<double>::quiet_NaN())));
    REQUIRE(rig.render() == RenderResult::Rendered);
    require_published(rig);

    status = read(state.status);
    REQUIRE(status.clients.at(0).commands_read == 3);
    REQUIRE(status.clients.at(0).last_sequence == valid.sequence());
    REQUIRE(status.clients.at(0).rejected == 0);
    REQUIRE(status.clients.at(0).last_rejection == 0);
    REQUIRE(status.clients.at(1).commands_read == 2);
    REQUIRE(status.clients.at(1).last_sequence == 2);
    REQUIRE(status.clients.at(1).rejected == 2);
    REQUIRE(status.clients.at(1).last_rejection == 2);
    REQUIRE(status.clients.at(2).rejected == 1);
    REQUIRE(status.clients.at(2).last_rejection == 4);

    // A detached slot.
    REQUIRE(rig.engine().detach(2));
    REQUIRE(rig.render() == RenderResult::Rendered);
    require_published(rig);
    REQUIRE((read(state.status).clients.at(2).flags & ks::ipc::kClientAttached) == 0);

    // A corrupt ring.
    ks::ipc::store_shared(valid.ring().head, 1'000'000, std::memory_order_release);
    REQUIRE(rig.render() == RenderResult::Rendered);
    require_published(rig);
    REQUIRE(read(state.status).clients.at(0).flags ==
            (ks::ipc::kClientAttached | ks::ipc::kClientCorrupt));
}

TEST_CASE("State: publishing doesn't change the output", "[state][engine]") {
    using enum CommandType;
    const auto first = ks::engine::test::music_track(0);
    const auto second = ks::engine::test::music_track(7'777);
    constexpr std::size_t kBlocks = 300;

    const auto run = [&](bool with_state) {
        EngineRig rig{with_state};
        Client client;
        REQUIRE(rig.engine().attach(client.ring()) == std::optional<std::size_t>{0});
        REQUIRE(rig.engine().load(0, first->track()));
        REQUIRE(rig.engine().load(1, second->track()));
        REQUIRE(client.send(assign(0, CrossfaderSide::A)));
        REQUIRE(client.send(assign(1, CrossfaderSide::B)));
        REQUIRE(client.send(global_command(SetCrossfader, 0.0)));
        REQUIRE(client.send(playing(0, true)));
        REQUIRE(client.send(playing(1, true)));

        std::vector<float> out;
        for (std::size_t block = 0; block < kBlocks; ++block) {
            switch (block) {
            case 50:
                REQUIRE(client.send(global_command(SetCrossfader, 0.25)));
                break;
            case 100:
                REQUIRE(client.send(deck_command(SetFader, 1, 0.6)));
                REQUIRE(client.send(global_command(SetCrossfader, 0.5)));
                break;
            case 150:
                REQUIRE(client.send(deck_command(SetRate, 1, 1.06)));
                break;
            case 200:
                REQUIRE(client.send(deck_command(Seek, 0, 10'000.0)));
                break;
            case 250:
                REQUIRE(client.send(global_command(SetCrossfader, 0.75)));
                break;
            default:
                break;
            }
            REQUIRE(rig.render() == RenderResult::Rendered);
            for (std::size_t c = 0; c < ks::engine::state_test::kChannels; ++c) {
                for (std::size_t f = 0; f < kBlock; ++f) {
                    out.push_back(rig.output().at(c, f));
                }
            }
        }
        if (with_state) {
            REQUIRE(read(rig.state().status).heartbeat == kBlocks);
        } else {
            REQUIRE(read(rig.state().status).heartbeat == 0);
        }
        return out;
    };

    const std::vector<float> without = run(false);
    const std::vector<float> with = run(true);
    REQUIRE(without.size() == with.size());
    bool sound = false;
    for (std::size_t i = 0; i < with.size(); ++i) {
        if (std::bit_cast<std::uint32_t>(with.at(i)) !=
            std::bit_cast<std::uint32_t>(without.at(i))) {
            CAPTURE(i);
            FAIL("the output differs");
        }
        sound = sound || with.at(i) != 0.0F;
    }
    REQUIRE(sound);
}
