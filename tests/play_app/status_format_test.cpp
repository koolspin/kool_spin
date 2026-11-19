// SPDX-License-Identifier: GPL-3.0-or-later

#include "status_format.h"

#include <ks/client/snapshot.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using ks::client::OutputState;
using ks::client::Rejection;
using ks::client::Side;
using ks::client::Snapshot;
using ks::play_app::format_status;

namespace {

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in{text};
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> format(const Snapshot& snapshot) {
    std::ostringstream out;
    format_status(snapshot, out);
    return lines_of(out.str());
}

} // namespace

TEST_CASE("Status lines: every field, in order", "[play_app][status]") {
    Snapshot snapshot{};
    snapshot.engine = {.heartbeat = 1234,
                       .frames_rendered = 99,
                       .sample_rate = 48'000,
                       .state = OutputState::Playing,
                       .muted = true,
                       .silenced = false,
                       .non_finite_samples = 5,
                       .limited_frames = 6,
                       .late_blocks = 7,
                       .render_load = 0.25,
                       .peak_render_load = 0.5};
    snapshot.decks[1] = {.loaded = true,
                         .playing = true,
                         .waiting = false,
                         .complete = true,
                         .track_id = 42,
                         .track_sample_rate = 44'100,
                         .track_frames = 44'100,
                         .frames_ready = 22'050,
                         .position = 1234.5,
                         .rate = 1.02,
                         .cue = 11'025.0};
    snapshot.channels[2] = {.trim_db = -3.0, .fader = 0.5, .side = Side::B, .cue = true};
    snapshot.channels[0].side = Side::A;
    snapshot.mixer = {.crossfader = 0.25,
                      .crossfader_transition = 0.5,
                      .main_level_db = -6.0,
                      .headphone_level_db = -9.0,
                      .cue_mix = 0.75,
                      .bus_main = 2,
                      .bus_cue = 4};
    snapshot.clients[3] = {.attached = true,
                           .corrupt = false,
                           .commands_read = 10,
                           .last_sequence = 9,
                           .rejected = 1,
                           .last_rejection = Rejection::BadValue};

    const std::vector<std::string> lines = format(snapshot);
    REQUIRE(lines.size() == 11);
    CHECK(lines[0] ==
          "engine heartbeat=1234 state=playing sample_rate=48000 muted=1 silenced=0 late_blocks=7 "
          "non_finite_samples=5 limited_frames=6 render_load=0.25 peak_render_load=0.5");
    const std::string empty_deck = " loaded=0 playing=0 waiting=0 complete=0 track=0 track_rate=0 "
                                   "track_frames=0 frames_ready=0 position=0 rate=0 cue=0";
    CHECK(lines[1] == "deck 0" + empty_deck);
    CHECK(lines[2] == "deck 1 loaded=1 playing=1 waiting=0 complete=1 track=42 track_rate=44100 "
                      "track_frames=44100 frames_ready=22050 position=1234.5 rate=1.02 cue=11025");
    CHECK(lines[3] == "deck 2" + empty_deck);
    CHECK(lines[4] == "deck 3" + empty_deck);
    CHECK(lines[5] == "channel 0 trim_db=0 fader=0 side=a cue=0");
    CHECK(lines[6] == "channel 1 trim_db=0 fader=0 side=thru cue=0");
    CHECK(lines[7] == "channel 2 trim_db=-3 fader=0.5 side=b cue=1");
    CHECK(lines[8] == "channel 3 trim_db=0 fader=0 side=thru cue=0");
    CHECK(lines[9] == "mixer crossfader=0.25 curve=0.5 main_db=-6 headphone_db=-9 cue_mix=0.75 "
                      "bus_main=2 bus_cue=4");
    CHECK(
        lines[10] ==
        "client 3 corrupt=0 commands_read=10 last_sequence=9 rejected=1 last_rejection=bad_value");
}

TEST_CASE("Status lines: the engine's states", "[play_app][status]") {
    Snapshot snapshot{};
    const auto engine_state = [&](OutputState state) {
        snapshot.engine.state = state;
        const std::string line = format(snapshot).at(0);
        const std::size_t begin = line.find(" state=") + 7;
        return line.substr(begin, line.find(' ', begin) - begin);
    };
    CHECK(engine_state(OutputState::Stopped) == "stopped");
    CHECK(engine_state(OutputState::Playing) == "playing");
    CHECK(engine_state(OutputState::Freewheeling) == "freewheeling");
    CHECK(engine_state(OutputState::Attaching) == "attaching");
}

TEST_CASE("Status lines: reals use %.6g, NaN and infinities included", "[play_app][status]") {
    Snapshot snapshot{};
    snapshot.mixer.crossfader = std::numeric_limits<double>::quiet_NaN();
    snapshot.mixer.main_level_db = -std::numeric_limits<double>::infinity();
    snapshot.mixer.headphone_level_db = std::numeric_limits<double>::infinity();
    snapshot.mixer.cue_mix = 1.0 / 3.0;
    snapshot.mixer.crossfader_transition = 1.0e-7;
    snapshot.decks[0].position = 123'456'789.0;
    const std::vector<std::string> lines = format(snapshot);
    REQUIRE(lines.size() == 10);
    CHECK(lines[9] == "mixer crossfader=nan curve=1e-07 main_db=-inf headphone_db=inf "
                      "cue_mix=0.333333 bus_main=0 bus_cue=0");
    CHECK(lines[1].find(" position=1.23457e+08 ") != std::string::npos);
}

TEST_CASE("Status lines: client slots attached or corrupt, rejections with _",
          "[play_app][status]") {
    Snapshot snapshot{};
    snapshot.clients[0] = {.attached = true, .last_rejection = Rejection::UnknownType};
    snapshot.clients[1] = {.attached = false, .rejected = 3, .last_rejection = Rejection::BadValue};
    snapshot.clients[2] = {.attached = false, .corrupt = true, .last_rejection = Rejection::None};
    snapshot.clients[5] = {.attached = true, .last_rejection = Rejection::BadTarget};
    snapshot.clients[7] = {.attached = true, .last_rejection = Rejection::NonzeroPadding};

    const std::vector<std::string> lines = format(snapshot);
    REQUIRE(lines.size() == 14);
    CHECK(lines[10] == "client 0 corrupt=0 commands_read=0 last_sequence=0 rejected=0 "
                       "last_rejection=unknown_type");
    CHECK(lines[11] == "client 2 corrupt=1 commands_read=0 last_sequence=0 rejected=0 "
                       "last_rejection=none");
    CHECK(lines[12] == "client 5 corrupt=0 commands_read=0 last_sequence=0 rejected=0 "
                       "last_rejection=bad_target");
    CHECK(lines[13] == "client 7 corrupt=0 commands_read=0 last_sequence=0 rejected=0 "
                       "last_rejection=nonzero_padding");
}
