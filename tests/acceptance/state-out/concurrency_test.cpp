// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/state-out/spec.md: readers on other threads see
// whole blocks while the engine renders (requirement 19).

#include <ks/engine/engine.h>
#include <ks/engine/render_core.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>

#include "../commands/test_support.h"
#include "test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

namespace {

using ks::engine::RenderResult;
using ks::engine::state_test::EngineRig;
using ks::engine::state_test::IdTrack;
using ks::engine::state_test::kBlock;
using ks::engine::state_test::read;
using ks::engine::state_test::real;
using ks::ipc::DeckState;
using ks::ipc::EngineStatus;
using ks::ipc::MixerState;
using ks::ipc::SeqlockReader;

constexpr std::uint64_t kTrackFrames = 480'000;

/// What one reader saw.
struct Seen {
    std::uint64_t reads = 0;
    std::uint64_t inconsistent = 0;
    std::uint64_t backwards = 0;
    std::uint64_t last_status = 0;
    std::uint64_t last_deck = 0;
    std::uint64_t last_mixer = 0;
};

/// Reads every block until `done`, checking each successful read.
void read_until(ks::ipc::EngineState& state, const std::atomic<bool>& done, Seen& seen) {
    const SeqlockReader<EngineStatus> status{state.status};
    const SeqlockReader<DeckState> deck{state.decks.at(0)};
    const SeqlockReader<MixerState> mixer{state.mixer};
    while (!done.load(std::memory_order_acquire)) {
        if (const std::optional<EngineStatus> s = status.try_read()) {
            ++seen.reads;
            if (s->frames_rendered != kBlock * s->heartbeat) {
                ++seen.inconsistent;
            }
            seen.backwards += s->heartbeat < seen.last_status ? 1 : 0;
            seen.last_status = s->heartbeat;
        }
        if (const std::optional<DeckState> d = deck.try_read()) {
            ++seen.reads;
            // Playing at rate 1.0 from the first block, then held at the end.
            const double expected = static_cast<double>(
                std::min(kBlock * d->heartbeat, static_cast<std::size_t>(kTrackFrames)));
            if (real(d->position) != expected) {
                ++seen.inconsistent;
            }
            seen.backwards += d->heartbeat < seen.last_deck ? 1 : 0;
            seen.last_deck = d->heartbeat;
        }
        if (const std::optional<MixerState> m = mixer.try_read()) {
            ++seen.reads;
            seen.backwards += m->heartbeat < seen.last_mixer ? 1 : 0;
            seen.last_mixer = m->heartbeat;
        }
    }
}

/// A thread that joins when destroyed. AppleClang 17's libc++ doesn't provide
/// std::jthread.
class JoiningThread {
public:
    template <typename F>
    explicit JoiningThread(F&& f) : thread_{std::forward<F>(f)} {}

    JoiningThread(const JoiningThread&) = delete;
    JoiningThread& operator=(const JoiningThread&) = delete;
    JoiningThread(JoiningThread&&) = delete;
    JoiningThread& operator=(JoiningThread&&) = delete;
    ~JoiningThread() { thread_.join(); }

private:
    std::thread thread_;
};

} // namespace

TEST_CASE("State: readers never see a torn block", "[state][concurrency]") {
    EngineRig rig;
    ks::engine::test::Client client;
    IdTrack track{kTrackFrames, 9};
    REQUIRE(rig.engine().attach(client.ring()) == std::optional<std::size_t>{0});
    REQUIRE(rig.engine().load(0, track.track()));
    REQUIRE(client.send(ks::engine::test::playing(0, true)));

    std::atomic<bool> done{false};
    std::array<Seen, 3> seen{};
    {
        const JoiningThread first{[&] { read_until(rig.state(), done, seen[0]); }};
        const JoiningThread second{[&] { read_until(rig.state(), done, seen[1]); }};
        const JoiningThread third{[&] { read_until(rig.state(), done, seen[2]); }};

        // This thread is the audio thread: it renders without pause.
        const auto stop = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < stop) {
            if (rig.render() != RenderResult::Rendered) {
                break;
            }
        }
        done.store(true, std::memory_order_release);
    }

    REQUIRE(read(rig.state().status).heartbeat == rig.blocks());
    REQUIRE(rig.blocks() > 100);
    for (const Seen& reader : seen) {
        CAPTURE(reader.reads, reader.inconsistent, reader.backwards, reader.last_status);
        REQUIRE(reader.inconsistent == 0);
        REQUIRE(reader.backwards == 0);
        REQUIRE(reader.reads >= 1'000);
        REQUIRE(reader.last_status > 0);
    }
}
