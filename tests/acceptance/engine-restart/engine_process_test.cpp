// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/engine-restart/spec.md: ks-engine on its own, as
// the supervisor would start it, on the paced backend (requirements 5, 7, 11,
// and 14 to 16). The test plays the supervisor: it creates the segments, and
// starts, kills, and restarts ks-engine on them.

#include <ks/ipc/command.h>
#include <ks/ipc/connection.h>
#include <ks/ipc/crc32.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/recording.h>
#include <ks/ipc/region.h>
#include <ks/ipc/segment.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>
#include <ks/ipc/track_segment.h>
#include <ks/supervisor/spawn.h>

#include "restart_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace ks::engine_restart::test;
using ks::ipc::Fd;
using ks::ipc::SegmentKind;
using ks::supervisor::Inherit;
namespace msg = ks::ipc::msg;

constexpr std::uint64_t kTrackId = 7;
constexpr std::uint64_t kSineFrames = std::uint64_t{kRate} * 20;

/// A track segment the test writes: a 440 Hz sine at half scale, 20 s long,
/// complete.
struct SineTrack {
    Fd fd;
    ks::ipc::Region region;
};

SineTrack make_sine_track(ks::ipc::SegmentFactory& factory) {
    std::expected<Fd, std::error_code> fd = factory.create(
        SegmentKind::Track, ks::ipc::kTrackLayoutVersion, ks::ipc::kTrackSegmentSize);
    REQUIRE(fd.has_value());
    std::expected<ks::ipc::Region, ks::ipc::MapFailure> region = ks::ipc::Region::map(
        fd->get(), SegmentKind::Track, ks::ipc::kTrackLayoutVersion, ks::ipc::Access::ReadWrite);
    REQUIRE(region.has_value());
    ks::ipc::TrackWriter writer{region->bytes()};
    writer.start(kRate);
    const std::span<float> samples = writer.samples();
    for (std::uint64_t n = 0; n < kSineFrames; ++n) {
        const auto value = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(n) / kRate));
        samples[static_cast<std::size_t>(n) * 2] = value;
        samples[(static_cast<std::size_t>(n) * 2) + 1] = value;
    }
    writer.finish(ks::ipc::TrackStatus::Complete, kSineFrames);
    return SineTrack{.fd = std::move(*fd), .region = std::move(*region)};
}

/// The test as supervisor: a runtime directory, the segments, and room for
/// the engines it starts on them, one after another.
class Harness {
public:
    explicit Harness(bool recording) {
        REQUIRE(!dir_.path().empty());
        std::expected<ks::ipc::SegmentFactory, std::error_code> factory =
            ks::ipc::SegmentFactory::open(dir_.path());
        REQUIRE(factory.has_value());
        factory_ = std::make_unique<ks::ipc::SegmentFactory>(std::move(*factory));
        std::expected<Fd, std::error_code> state =
            factory_->create(SegmentKind::EngineState, ks::ipc::kEngineStateLayoutVersion,
                             ks::ipc::kEngineStateSegmentSize);
        REQUIRE(state.has_value());
        state_ = std::move(*state);
        std::expected<ks::ipc::Region, ks::ipc::MapFailure> region =
            ks::ipc::Region::map(state_.get(), SegmentKind::EngineState,
                                 ks::ipc::kEngineStateLayoutVersion, ks::ipc::Access::ReadWrite);
        REQUIRE(region.has_value());
        state_region_ = std::make_unique<ks::ipc::Region>(std::move(*region));
        engine_state_ = state_region_->at<ks::ipc::EngineState>(ks::ipc::kSegmentHeaderSize);
        REQUIRE(engine_state_ != nullptr);
        std::expected<State, std::string> view = State::map(state_.get());
        REQUIRE(view.has_value());
        view_ = std::make_unique<State>(std::move(*view));
        if (recording) {
            std::expected<Fd, std::error_code> record =
                factory_->create(SegmentKind::Recording, ks::ipc::kRecordingLayoutVersion,
                                 ks::ipc::kRecordingSegmentSize);
            REQUIRE(record.has_value());
            record_ = std::move(*record);
        }
    }

    [[nodiscard]] ks::ipc::SegmentFactory& factory() const { return *factory_; }
    [[nodiscard]] ks::ipc::EngineState& engine_state() const { return *engine_state_; }
    [[nodiscard]] const State& view() const { return *view_; }
    [[nodiscard]] Process& engine() noexcept { return engine_; }
    [[nodiscard]] ks::ipc::Connection& socket() const { return *socket_; }

    /// Starts an engine on the segments, with `tracks` as `--track` options
    /// (each one's ID and descriptor), and `more` after the options. The old
    /// engine, if any, is waited for. Returns without waiting for Ready.
    void launch(const std::vector<std::pair<std::uint64_t, const Fd*>>& tracks = {},
                const std::vector<std::string>& more = {}) {
        std::expected<std::pair<Fd, Fd>, std::error_code> pair = ks::ipc::socket_pair();
        REQUIRE(pair.has_value());
        std::vector<std::string> args{"--supervisor-fd",   "3", "--state-fd", "4", "--runtime-dir",
                                      dir_.path().string()};
        std::vector<Inherit> fds{{.parent = pair->second.get(), .child = 3},
                                 {.parent = state_.get(), .child = 4}};
        if (record_.valid()) {
            args.insert(args.end(), {"--record-fd", "5"});
            fds.push_back({.parent = record_.get(), .child = 5});
        }
        int next = 7;
        for (const auto& [id, fd] : tracks) {
            args.insert(args.end(), {"--track", std::to_string(id) + ":" + std::to_string(next)});
            fds.push_back({.parent = fd->get(), .child = next});
            ++next;
        }
        args.insert(args.end(), {"--backend", "paced", "--rate", std::to_string(kRate),
                                 "--channels", "2", "--period", std::to_string(kPeriod)});
        args.insert(args.end(), more.begin(), more.end());
        engine_ = ks::processes::test::start(ks::processes::test::kEnginePath, args, fds);
        REQUIRE(engine_.pid() > 0);
        pair->second.reset();
        REQUIRE(ks::ipc::test::set_nonblocking(pair->first.get()));
        socket_ = std::make_unique<ks::ipc::Connection>(std::move(pair->first));
    }

    /// launch(), then waits for Ready.
    void start(const std::vector<std::pair<std::uint64_t, const Fd*>>& tracks = {},
               const std::vector<std::string>& more = {}) {
        launch(tracks, more);
        REQUIRE(ks::processes::test::is<msg::Ready>(ks::processes::test::receive(*socket_), 0));
    }

    /// Kills the engine with SIGKILL and waits for it.
    void kill_engine() {
        engine_.signal(SIGKILL);
        REQUIRE(engine_.wait().has_value());
    }

    /// Maps the recording segment read-only.
    [[nodiscard]] std::unique_ptr<ks::ipc::Region> map_recording() const {
        REQUIRE(record_.valid());
        std::expected<ks::ipc::Region, ks::ipc::MapFailure> region =
            ks::ipc::Region::map(record_.get(), SegmentKind::Recording,
                                 ks::ipc::kRecordingLayoutVersion, ks::ipc::Access::ReadOnly);
        REQUIRE(region.has_value());
        return std::make_unique<ks::ipc::Region>(std::move(*region));
    }

    /// Attaches a command ring in slot 0, and loads `track` as `id` on deck 0.
    /// Returns the ring.
    std::unique_ptr<ks::client_rings::test::ClientRing> attach_and_load(const SineTrack& track,
                                                                        std::uint64_t id) {
        std::expected<Fd, std::error_code> segment =
            factory_->create(SegmentKind::CommandRing, ks::ipc::kCommandRingLayoutVersion,
                             ks::ipc::kCommandRingSegmentSize);
        REQUIRE(segment.has_value());
        std::array<int, 1> fds{segment->get()};
        REQUIRE(ks::processes::test::send(
            *socket_, ks::ipc::Message{.request_id = 1, .body = msg::Attach{.slot = 0}}, fds));
        REQUIRE(ks::processes::test::is<msg::Attached>(ks::processes::test::receive(*socket_), 1));
        std::expected<ks::client_rings::test::ClientRing, std::string> ring =
            ks::client_rings::test::ClientRing::map(segment->get());
        REQUIRE(ring.has_value());

        fds[0] = track.fd.get();
        REQUIRE(ks::processes::test::send(
            *socket_,
            ks::ipc::Message{.request_id = 2,
                             .body =
                                 msg::LoadTrack{.track_id = id, .deck = 0, .sample_rate = kRate}},
            fds));
        REQUIRE(
            ks::processes::test::is<msg::TrackLoaded>(ks::processes::test::receive(*socket_), 2));
        return std::make_unique<ks::client_rings::test::ClientRing>(std::move(*ring));
    }

private:
    ks::ipc::test::TempDir dir_;
    ks::processes::test::EngineReaper reaper_{dir_.path()};
    std::unique_ptr<ks::ipc::SegmentFactory> factory_;
    Fd state_;
    Fd record_;
    std::unique_ptr<ks::ipc::Region> state_region_;
    ks::ipc::EngineState* engine_state_ = nullptr;
    std::unique_ptr<State> view_;
    std::unique_ptr<ks::ipc::Connection> socket_;
    Process engine_;
};

/// True if, within 5 s, deck 0 is loaded with `id` and playing.
bool deck_resumed(const Harness& harness, std::uint64_t id) {
    return eventually([&] {
        const auto deck = harness.view().deck(0);
        return deck && loaded(*deck) && deck->track_id == id && playing(*deck);
    });
}

/// True if the heartbeat grows over 300 ms.
bool beats(const Harness& harness) {
    return eventually([&] {
        const std::uint64_t before = harness.view().heartbeat();
        std::this_thread::sleep_for(Milliseconds{300});
        return before != 0 && harness.view().heartbeat() > before;
    });
}

/// Plays `id` on deck 0 for a second on a first engine, which is then
/// killed, leaving a resume state behind.
void leave_a_playing_deck(Harness& harness, const SineTrack& track) {
    harness.start();
    const auto ring = harness.attach_and_load(track, kTrackId);
    REQUIRE(ring->push(ks::track_loading::test::set_playing(0, true, 1)));
    REQUIRE(eventually([&] {
        const auto deck = harness.view().deck(0);
        return deck && playing(*deck) && real_of(deck->position) > 0.5 * kRate;
    }));
    harness.kill_engine();
}

} // namespace

TEST_CASE("Engine process: paced backend records", "[engine-process]") {
    Harness harness{true};
    const SineTrack track = make_sine_track(harness.factory());
    harness.start();
    const auto recording_region = harness.map_recording();
    auto* const segment =
        recording_region->at<ks::ipc::RecordingSegment>(ks::ipc::kSegmentHeaderSize);
    REQUIRE(segment != nullptr);
    const ks::ipc::RecordingReader reader{*segment};

    const auto ring = harness.attach_and_load(track, kTrackId);
    std::this_thread::sleep_for(Milliseconds{400});
    REQUIRE(ring->push(ks::track_loading::test::set_playing(0, true, 1)));
    REQUIRE(eventually([&] {
        const auto deck = harness.view().deck(0);
        return deck && playing(*deck) && real_of(deck->position) > 1.5 * kRate;
    }));

    CHECK(reader.starts() == 1);
    REQUIRE(reader.boundaries().size() == 1);
    CHECK(reader.boundaries().front().frame == 0);
    const std::uint64_t written = reader.frames_written();
    REQUIRE(written > 2 * kRate);
    const auto frames = reader.read(0, written);
    REQUIRE(frames.has_value());
    const std::vector<float> all = frames.value_or(std::vector<float>{});
    std::vector<float> left;
    for (std::size_t i = 0; i < all.size(); i += 2) {
        left.push_back(all[i]);
    }

    // Silence, then the sine.
    const auto first_sound =
        std::ranges::find_if(left, [](float x) { return std::abs(x) > 1e-4F; });
    REQUIRE(first_sound != left.end());
    const auto started = static_cast<std::size_t>(first_sound - left.begin());
    CHECK(static_cast<double>(started) > 0.2 * kRate);
    CHECK(std::ranges::all_of(std::span{left}.first(started),
                              [](float x) { return std::abs(x) <= 1e-4F; }));
    // No frame missing: after the 50 ms the safety stage takes to come up, the
    // sine's zero crossings are one period apart within a frame.
    const auto steady = std::span{left}.subspan(started + static_cast<std::size_t>(0.1 * kRate));
    const GapCheck check = check_no_gap(steady, 300);
    CAPTURE(check.crossings, check.widest, check.narrowest);
    CHECK(check.ok);
}

TEST_CASE("Engine process: --track", "[engine-process]") {
    SECTION("a valid track resumes its deck") {
        Harness harness{false};
        const SineTrack track = make_sine_track(harness.factory());
        leave_a_playing_deck(harness, track);
        harness.start({{kTrackId, &track.fd}});
        CHECK(deck_resumed(harness, kTrackId));
        CHECK(beats(harness));
    }

    SECTION("a segment that isn't a track is skipped, and its deck comes back empty") {
        Harness harness{false};
        const SineTrack track = make_sine_track(harness.factory());
        leave_a_playing_deck(harness, track);
        std::expected<Fd, std::error_code> wrong =
            harness.factory().create(SegmentKind::CommandRing, ks::ipc::kCommandRingLayoutVersion,
                                     ks::ipc::kCommandRingSegmentSize);
        REQUIRE(wrong.has_value());
        harness.start({{kTrackId, &*wrong}});
        CHECK(beats(harness));
        CHECK(harness.engine().running());
        const auto deck = require(harness.view().deck(0));
        CHECK_FALSE(loaded(deck));
        // The engine says why, on standard error, and never plays anything.
        CHECK_FALSE(playing(deck));
    }

    SECTION("a track that no deck holds is released") {
        Harness harness{false};
        const SineTrack track = make_sine_track(harness.factory());
        leave_a_playing_deck(harness, track);
        const SineTrack other = make_sine_track(harness.factory());
        // Track 8 isn't on any deck in the resume state.
        harness.launch({{kTrackId, &track.fd}, {8, &other.fd}});
        bool ready = false;
        bool released = false;
        for (int i = 0; i < 4 && !(ready && released); ++i) {
            const auto received = ks::processes::test::receive(harness.socket());
            if (!received) {
                break;
            }
            ready = ready || std::holds_alternative<msg::Ready>(received->message.body);
            if (const auto* const note = std::get_if<msg::TrackReleased>(&received->message.body)) {
                released = released || note->track_id == 8;
                CHECK(note->track_id != kTrackId);
            }
        }
        CHECK(ready);
        CHECK(released);
        CHECK(deck_resumed(harness, kTrackId));
    }

    SECTION("what is a usage error") {
        // Parsing comes first, so the descriptors can be anything open.
        const ks::ipc::test::TempDir dir;
        std::expected<ks::ipc::SegmentFactory, std::error_code> factory =
            ks::ipc::SegmentFactory::open(dir.path());
        REQUIRE(factory.has_value());
        const SineTrack track = make_sine_track(*factory);
        const auto refused = [&](const std::vector<std::string>& more) {
            std::vector<std::string> args{
                "--supervisor-fd", "3",         "--state-fd", "4", "--runtime-dir",
                "/nonexistent",    "--backend", "paced"};
            args.insert(args.end(), more.begin(), more.end());
            const std::vector<Inherit> fds{{.parent = track.fd.get(), .child = 3},
                                           {.parent = track.fd.get(), .child = 4},
                                           {.parent = track.fd.get(), .child = 7}};
            Process engine =
                ks::processes::test::start(ks::processes::test::kEnginePath, args, fds);
            return engine.wait() == 2;
        };
        CHECK(refused({"--track", "7:7", "--track", "7:8"})); // a repeated ID
        CHECK(refused({"--track", "7:7", "--track", "8:7"})); // a repeated descriptor
        CHECK(refused({"--track", "0:7"}));                   // an ID of 0
        CHECK(refused({"--track", "7"}));                     // no descriptor
        CHECK(refused({"--track", "x:7"}));                   // not a number
        CHECK(refused({"--track", "7:-1"}));                  // a bad descriptor
        CHECK(refused({"--clean", "--clean"}));               // repeated
    }
}

TEST_CASE("Engine process: corrupted state starts clean", "[engine-process]") {
    Harness harness{false};
    const SineTrack track = make_sine_track(harness.factory());
    leave_a_playing_deck(harness, track);
    ks::ipc::EngineState& state = harness.engine_state();

    // What a resume slot held, to corrupt copies of.
    const auto slot_value = [&](std::size_t slot) {
        return ks::ipc::SeqlockReader<ks::ipc::ResumeState>{std::span{state.resume}[slot]}.try_read(
            1'000);
    };
    const auto write_slot = [&](std::size_t slot, const ks::ipc::ResumeState& value) {
        ks::ipc::SeqlockWriter<ks::ipc::ResumeState> writer{std::span{state.resume}[slot]};
        writer.write(value);
    };
    const auto sealed = [](ks::ipc::ResumeState value) {
        value.checksum = ks::ipc::crc32(
            std::as_bytes(std::span{&value, 1}).first(offsetof(ks::ipc::ResumeState, checksum)));
        return value;
    };

    SECTION("a flipped bit fails the CRC") {
        for (std::size_t slot = 0; slot < ks::ipc::kResumeSlots; ++slot) {
            auto value = require(slot_value(slot));
            value.host_time ^= 1;
            write_slot(slot, value);
        }
    }

    SECTION("a rate that is out of range, with a good CRC") {
        for (std::size_t slot = 0; slot < ks::ipc::kResumeSlots; ++slot) {
            auto value = require(slot_value(slot));
            value.decks[0].rate = std::bit_cast<std::uint64_t>(1e30);
            write_slot(slot, sealed(value));
        }
    }

    SECTION("a position that is NaN, with a good CRC") {
        for (std::size_t slot = 0; slot < ks::ipc::kResumeSlots; ++slot) {
            auto value = require(slot_value(slot));
            value.decks[0].position =
                std::bit_cast<std::uint64_t>(std::numeric_limits<double>::quiet_NaN());
            write_slot(slot, sealed(value));
        }
    }

    SECTION("random bytes in the whole segment") {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): scribbling on the segment
        auto* const raw = reinterpret_cast<std::byte*>(&state);
        const std::span<std::byte> bytes{raw, sizeof(ks::ipc::EngineState)};
        std::uint64_t random = 0x9E3779B97F4A7C15ULL;
        for (std::byte& byte : bytes) {
            random ^= random << 13U;
            random ^= random >> 7U;
            random ^= random << 17U;
            byte = static_cast<std::byte>(random);
        }
    }

    harness.start({{kTrackId, &track.fd}});
    // It started, it says so, and it plays nothing: decks paused and empty.
    CHECK(beats(harness));
    CHECK(harness.engine().running());
    const auto deck = require(harness.view().deck(0));
    CHECK_FALSE(loaded(deck));
    CHECK_FALSE(playing(deck));
    harness.engine().signal(SIGTERM);
    CHECK(harness.engine().wait() == 0);
}
