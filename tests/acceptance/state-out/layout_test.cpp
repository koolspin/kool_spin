// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/state-out/spec.md: the engine-state layout
// (requirements 1 to 8).

#include <ks/engine/engine.h>
#include <ks/engine/mixer.h>
#include <ks/ipc/region.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>

#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace {

using ks::ipc::ChannelState;
using ks::ipc::ClientAck;
using ks::ipc::DeckState;
using ks::ipc::EngineState;
using ks::ipc::EngineStatus;
using ks::ipc::MixerState;
using ks::ipc::Seqlock;

constexpr std::size_t kWord = sizeof(std::uint64_t);

template <typename T>
constexpr bool kSharedLayout = std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>;

} // namespace

TEST_CASE("State: the layout", "[state][layout]") {
    STATIC_REQUIRE(kSharedLayout<DeckState>);
    STATIC_REQUIRE(kSharedLayout<ChannelState>);
    STATIC_REQUIRE(kSharedLayout<MixerState>);
    STATIC_REQUIRE(kSharedLayout<ClientAck>);
    STATIC_REQUIRE(kSharedLayout<EngineStatus>);
    STATIC_REQUIRE(kSharedLayout<EngineState>);

    STATIC_REQUIRE(ks::ipc::kStateDecks == ks::engine::kDecks);
    STATIC_REQUIRE(ks::ipc::kStateClients == ks::engine::kMaxClients);
    STATIC_REQUIRE(ks::ipc::kEngineStateLayoutVersion == 2);

    // Requirement 3: each field 8 bytes after the last, from 0.
    STATIC_REQUIRE(sizeof(DeckState) == 11 * kWord);
    STATIC_REQUIRE(offsetof(DeckState, heartbeat) == 0 * kWord);
    STATIC_REQUIRE(offsetof(DeckState, flags) == 1 * kWord);
    STATIC_REQUIRE(offsetof(DeckState, track_id) == 2 * kWord);
    STATIC_REQUIRE(offsetof(DeckState, track_sample_rate) == 3 * kWord);
    STATIC_REQUIRE(offsetof(DeckState, track_frames) == 4 * kWord);
    STATIC_REQUIRE(offsetof(DeckState, frames_ready) == 5 * kWord);
    STATIC_REQUIRE(offsetof(DeckState, position) == 6 * kWord);
    STATIC_REQUIRE(offsetof(DeckState, rate) == 7 * kWord);
    STATIC_REQUIRE(offsetof(DeckState, cue) == 8 * kWord);
    STATIC_REQUIRE(offsetof(DeckState, host_time) == 9 * kWord);
    STATIC_REQUIRE(offsetof(DeckState, output_latency) == 10 * kWord);

    // Requirement 4.
    STATIC_REQUIRE(sizeof(ChannelState) == 4 * kWord);
    STATIC_REQUIRE(offsetof(ChannelState, trim_db) == 0 * kWord);
    STATIC_REQUIRE(offsetof(ChannelState, fader) == 1 * kWord);
    STATIC_REQUIRE(offsetof(ChannelState, side) == 2 * kWord);
    STATIC_REQUIRE(offsetof(ChannelState, cue) == 3 * kWord);
    STATIC_REQUIRE(sizeof(MixerState) == 24 * kWord);
    STATIC_REQUIRE(offsetof(MixerState, heartbeat) == 0 * kWord);
    STATIC_REQUIRE(offsetof(MixerState, channels) == 1 * kWord);
    STATIC_REQUIRE(offsetof(MixerState, crossfader) == 17 * kWord);
    STATIC_REQUIRE(offsetof(MixerState, crossfader_transition) == 18 * kWord);
    STATIC_REQUIRE(offsetof(MixerState, main_level_db) == 19 * kWord);
    STATIC_REQUIRE(offsetof(MixerState, headphone_level_db) == 20 * kWord);
    STATIC_REQUIRE(offsetof(MixerState, cue_mix) == 21 * kWord);
    STATIC_REQUIRE(offsetof(MixerState, bus_main) == 22 * kWord);
    STATIC_REQUIRE(offsetof(MixerState, bus_cue) == 23 * kWord);

    // Requirement 5.
    STATIC_REQUIRE(sizeof(ClientAck) == 5 * kWord);
    STATIC_REQUIRE(offsetof(ClientAck, flags) == 0 * kWord);
    STATIC_REQUIRE(offsetof(ClientAck, commands_read) == 1 * kWord);
    STATIC_REQUIRE(offsetof(ClientAck, last_sequence) == 2 * kWord);
    STATIC_REQUIRE(offsetof(ClientAck, rejected) == 3 * kWord);
    STATIC_REQUIRE(offsetof(ClientAck, last_rejection) == 4 * kWord);

    // Requirement 6.
    STATIC_REQUIRE(sizeof(EngineStatus) == (13 + 40) * kWord);
    STATIC_REQUIRE(offsetof(EngineStatus, heartbeat) == 0 * kWord);
    STATIC_REQUIRE(offsetof(EngineStatus, frames_rendered) == 1 * kWord);
    STATIC_REQUIRE(offsetof(EngineStatus, sample_rate) == 2 * kWord);
    STATIC_REQUIRE(offsetof(EngineStatus, host_time) == 3 * kWord);
    STATIC_REQUIRE(offsetof(EngineStatus, output_latency) == 4 * kWord);
    STATIC_REQUIRE(offsetof(EngineStatus, output_state) == 5 * kWord);
    STATIC_REQUIRE(offsetof(EngineStatus, flags) == 6 * kWord);
    STATIC_REQUIRE(offsetof(EngineStatus, non_finite_samples) == 7 * kWord);
    STATIC_REQUIRE(offsetof(EngineStatus, limited_frames) == 8 * kWord);
    STATIC_REQUIRE(offsetof(EngineStatus, late_blocks) == 9 * kWord);
    STATIC_REQUIRE(offsetof(EngineStatus, render_time) == 10 * kWord);
    STATIC_REQUIRE(offsetof(EngineStatus, render_load) == 11 * kWord);
    STATIC_REQUIRE(offsetof(EngineStatus, peak_render_load) == 12 * kWord);
    STATIC_REQUIRE(offsetof(EngineStatus, clients) == 13 * kWord);

    // Requirement 7: decks, then the mixer, then the status.
    STATIC_REQUIRE(std::is_same_v<decltype(EngineState::decks),
                                  std::array<Seqlock<DeckState>, ks::ipc::kStateDecks>>);
    STATIC_REQUIRE(std::is_same_v<decltype(EngineState::mixer), Seqlock<MixerState>>);
    STATIC_REQUIRE(std::is_same_v<decltype(EngineState::status), Seqlock<EngineStatus>>);
    STATIC_REQUIRE(offsetof(EngineState, decks) == 0);
    STATIC_REQUIRE(offsetof(EngineState, mixer) > offsetof(EngineState, decks));
    STATIC_REQUIRE(offsetof(EngineState, status) > offsetof(EngineState, mixer));
    STATIC_REQUIRE(ks::ipc::kEngineStateSegmentSize ==
                   ks::ipc::kSegmentHeaderSize + sizeof(EngineState));

    // A segment of that size holds a header and an EngineState after it.
    auto region = ks::ipc::Region::create_anonymous(ks::ipc::kEngineStateSegmentSize);
    REQUIRE(region.has_value());
    ks::ipc::write_header(region->bytes(), ks::ipc::SegmentKind::EngineState,
                          ks::ipc::kEngineStateLayoutVersion);
    REQUIRE(ks::ipc::check_header(region->bytes(), ks::ipc::SegmentKind::EngineState,
                                  ks::ipc::kEngineStateLayoutVersion)
                .has_value());
    auto* const state = region->at<EngineState>(ks::ipc::kSegmentHeaderSize);
    REQUIRE(state != nullptr);

    // Requirement 8: zero-filled memory is a state in which nothing has been
    // published.
    using ks::engine::state_test::all_zero;
    using ks::engine::state_test::read;
    for (Seqlock<DeckState>& deck : state->decks) {
        REQUIRE(all_zero(read(deck)));
    }
    REQUIRE(all_zero(read(state->mixer)));
    REQUIRE(all_zero(read(state->status)));
}
