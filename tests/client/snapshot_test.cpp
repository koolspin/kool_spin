// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/client/snapshot.h>

#include <ks/client/session.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace {

namespace client = ks::client;
namespace ipc = ks::ipc;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

std::uint64_t bits(double value) {
    return std::bit_cast<std::uint64_t>(value);
}

template <typename T>
void publish(ipc::Seqlock<T>& lock, const T& value) {
    ipc::SeqlockWriter<T> writer{lock};
    writer.write(value);
}

ipc::MixerState valid_mixer() {
    ipc::MixerState mixer{};
    mixer.heartbeat = 1;
    for (auto& channel : mixer.channels) {
        channel.side = 2;
    }
    return mixer;
}

ipc::EngineStatus valid_status() {
    ipc::EngineStatus status{};
    status.heartbeat = 1;
    status.output_state = 1;
    return status;
}

std::unique_ptr<ipc::EngineState> valid_state() {
    auto state = std::make_unique<ipc::EngineState>();
    for (auto& deck : state->decks) {
        ipc::DeckState value{};
        value.heartbeat = 1;
        publish(deck, value);
    }
    publish(state->mixer, valid_mixer());
    publish(state->status, valid_status());
    return state;
}

/// The error text of snapshot_from(), which must fail with BadState.
std::string bad_state_text(ipc::EngineState& state) {
    const client::Result<client::Snapshot> snap = client::Session::snapshot_from(state);
    REQUIRE_FALSE(snap.has_value());
    CHECK(snap.error().kind == client::ErrorKind::BadState);
    return snap.error().text;
}

} // namespace

TEST_CASE("snapshot_from: every field of a valid state", "[client]") {
    const std::unique_ptr<ipc::EngineState> state = valid_state();

    ipc::DeckState deck{};
    deck.heartbeat = 7;
    deck.flags = ipc::kDeckLoaded | ipc::kDeckComplete;
    deck.track_id = 11;
    deck.track_sample_rate = 44'100;
    deck.track_frames = 1'000;
    deck.frames_ready = 900;
    deck.position = bits(12.5);
    deck.rate = bits(1.02);
    deck.cue = bits(3.0);
    publish(state->decks[1], deck);

    ipc::MixerState mixer = valid_mixer();
    mixer.channels[0].side = 0;
    mixer.channels[0].trim_db = bits(-3.0);
    mixer.channels[0].fader = bits(0.75);
    mixer.channels[0].cue = 1;
    mixer.channels[1].side = 1;
    mixer.crossfader = bits(0.25);
    mixer.crossfader_transition = bits(0.5);
    mixer.main_level_db = bits(-6.0);
    mixer.headphone_level_db = bits(-9.0);
    mixer.cue_mix = bits(0.4);
    mixer.bus_main = 2;
    mixer.bus_cue = 4;
    publish(state->mixer, mixer);

    ipc::EngineStatus status = valid_status();
    status.heartbeat = 42;
    status.frames_rendered = 4'200;
    status.sample_rate = 48'000;
    status.output_state = 3;
    status.flags = ipc::kStatusMuted | ipc::kStatusSilenced;
    status.non_finite_samples = 5;
    status.limited_frames = 6;
    status.late_blocks = 7;
    status.render_load = bits(0.125);
    status.peak_render_load = bits(0.5);
    status.clients[3].flags = ipc::kClientAttached;
    status.clients[3].commands_read = 10;
    status.clients[3].last_sequence = 9;
    status.clients[3].rejected = 2;
    status.clients[3].last_rejection = 4;
    status.clients[5].flags = ipc::kClientCorrupt;
    publish(state->status, status);

    const client::Result<client::Snapshot> snap = client::Session::snapshot_from(*state);
    REQUIRE(snap.has_value());
    const client::Snapshot s = snap.value_or(client::Snapshot{});

    CHECK(s.engine == client::EngineInfo{.heartbeat = 42,
                                         .frames_rendered = 4'200,
                                         .sample_rate = 48'000,
                                         .state = client::OutputState::Attaching,
                                         .muted = true,
                                         .silenced = true,
                                         .non_finite_samples = 5,
                                         .limited_frames = 6,
                                         .late_blocks = 7,
                                         .render_load = 0.125,
                                         .peak_render_load = 0.5});
    CHECK(s.decks[0] == client::DeckInfo{});
    CHECK(s.decks[1] == client::DeckInfo{.loaded = true,
                                         .playing = false,
                                         .waiting = false,
                                         .complete = true,
                                         .track_id = 11,
                                         .track_sample_rate = 44'100,
                                         .track_frames = 1'000,
                                         .frames_ready = 900,
                                         .position = 12.5,
                                         .rate = 1.02,
                                         .cue = 3.0});
    CHECK(
        s.channels[0] ==
        client::ChannelInfo{.trim_db = -3.0, .fader = 0.75, .side = client::Side::A, .cue = true});
    CHECK(s.channels[1].side == client::Side::B);
    CHECK(s.channels[2].side == client::Side::Thru);
    CHECK(s.mixer == client::MixerInfo{.crossfader = 0.25,
                                       .crossfader_transition = 0.5,
                                       .main_level_db = -6.0,
                                       .headphone_level_db = -9.0,
                                       .cue_mix = 0.4,
                                       .bus_main = 2,
                                       .bus_cue = 4});
    CHECK(s.clients[3] == client::ClientInfo{.attached = true,
                                             .corrupt = false,
                                             .commands_read = 10,
                                             .last_sequence = 9,
                                             .rejected = 2,
                                             .last_rejection = client::Rejection::BadValue});
    CHECK(s.clients[5] == client::ClientInfo{.attached = false, .corrupt = true});
    CHECK(s.clients[0] == client::ClientInfo{});
}

TEST_CASE("snapshot_from: deck flags", "[client]") {
    const std::unique_ptr<ipc::EngineState> state = valid_state();
    ipc::DeckState deck{};
    deck.flags = ipc::kDeckPlaying | ipc::kDeckWaiting;
    publish(state->decks[3], deck);
    const client::Result<client::Snapshot> snap = client::Session::snapshot_from(*state);
    REQUIRE(snap.has_value());
    const client::DeckInfo d = snap.value_or(client::Snapshot{}).decks[3];
    CHECK_FALSE(d.loaded);
    CHECK(d.playing);
    CHECK(d.waiting);
    CHECK_FALSE(d.complete);
}

TEST_CASE("snapshot_from: NaN and infinities are kept", "[client]") {
    const std::unique_ptr<ipc::EngineState> state = valid_state();
    ipc::DeckState deck{};
    deck.position = bits(kNaN);
    deck.rate = bits(kInf);
    deck.cue = bits(-kInf);
    publish(state->decks[0], deck);
    ipc::MixerState mixer = valid_mixer();
    mixer.main_level_db = bits(-kInf);
    mixer.channels[2].trim_db = bits(kNaN);
    publish(state->mixer, mixer);
    ipc::EngineStatus status = valid_status();
    status.render_load = bits(kInf);
    publish(state->status, status);

    const client::Result<client::Snapshot> snap = client::Session::snapshot_from(*state);
    REQUIRE(snap.has_value());
    const client::Snapshot s = snap.value_or(client::Snapshot{});
    CHECK(bits(s.decks[0].position) == bits(kNaN));
    CHECK(s.decks[0].rate == kInf);
    CHECK(s.decks[0].cue == -kInf);
    CHECK(s.mixer.main_level_db == -kInf);
    CHECK(std::isnan(s.channels[2].trim_db));
    CHECK(s.engine.render_load == kInf);
}

TEST_CASE("snapshot_from: invalid codes", "[client]") {
    const std::unique_ptr<ipc::EngineState> state = valid_state();

    SECTION("output_state") {
        ipc::EngineStatus status = valid_status();
        status.output_state = 4;
        publish(state->status, status);
        CHECK(bad_state_text(*state).find("output_state") != std::string::npos);
    }
    SECTION("a channel's side") {
        ipc::MixerState mixer = valid_mixer();
        mixer.channels[2].side = 3;
        publish(state->mixer, mixer);
        CHECK(bad_state_text(*state).find("channel 2 side") != std::string::npos);
    }
    SECTION("a client's last_rejection") {
        ipc::EngineStatus status = valid_status();
        status.clients[6].last_rejection = 5;
        publish(state->status, status);
        CHECK(bad_state_text(*state).find("client 6 last_rejection") != std::string::npos);
    }
}

TEST_CASE("snapshot_from: an unstable seqlock", "[client]") {
    const std::unique_ptr<ipc::EngineState> state = valid_state();

    SECTION("status") {
        state->status.sequence |= 1U;
        CHECK(bad_state_text(*state).find("status") != std::string::npos);
    }
    SECTION("a deck") {
        state->decks[2].sequence |= 1U;
        CHECK(bad_state_text(*state).find("deck 2") != std::string::npos);
    }
    SECTION("the mixer, within 100 ms") {
        state->mixer.sequence |= 1U;
        const auto start = std::chrono::steady_clock::now();
        CHECK(bad_state_text(*state).find("mixer") != std::string::npos);
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds{100});
    }
}
