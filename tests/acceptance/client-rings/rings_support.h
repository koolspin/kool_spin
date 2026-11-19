// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the client-rings acceptance tests: command slots, the engine's
// state as a client sees it, a client's ring, ks-engine with the test as its
// supervisor, and kool_spin with clients. Built on the processes tests'
// helpers (processes_support.h).

#include <ks/ipc/command.h>
#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/fd_passing.h>
#include <ks/ipc/layout.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/region.h>
#include <ks/ipc/runtime_dir.h>
#include <ks/ipc/segment.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/spsc_ring.h>
#include <ks/ipc/state.h>
#include <ks/supervisor/spawn.h>

#include "processes_support.h"
#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace ks::client_rings::test {

using processes::test::Clock;
using processes::test::eventually;
using processes::test::Milliseconds;
using processes::test::Process;
using processes::test::Received;

/// How long a command may take to be applied, and how long a client keeps
/// asking for a ring while the answer is Unavailable (the spec's 2 s).
inline constexpr Milliseconds kApplyWait{2'000};

// ---- Command slots ----------------------------------------------------------

/// The bit pattern of `value`, as shared memory carries reals.
inline std::uint64_t real_code(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

/// A slot for a command of `type` with target 0 and `value` in payload[0].
inline ipc::CommandSlot command(ipc::CommandType type, double value, std::uint64_t sequence) {
    return ipc::CommandSlot{.sequence = sequence,
                            .type = static_cast<std::uint32_t>(type),
                            .target = 0,
                            .payload = {real_code(value), 0, 0, 0, 0, 0}};
}

/// SetCrossfader to `position`.
inline ipc::CommandSlot set_crossfader(double position, std::uint64_t sequence) {
    return command(ipc::CommandType::SetCrossfader, position, sequence);
}

/// SetMainLevel to `db`.
inline ipc::CommandSlot set_main_level(double db, std::uint64_t sequence) {
    return command(ipc::CommandType::SetMainLevel, db, sequence);
}

// ---- The engine's state, as a client sees it ----------------------------------

/// An engine-state segment, mapped read-only.
class EngineView {
public:
    /// Maps `fd` as an engine-state segment, or says why it couldn't.
    static std::expected<EngineView, std::string> map(int fd) {
        std::expected<ipc::Region, ipc::MapFailure> region =
            ipc::Region::map(fd, ipc::SegmentKind::EngineState, ipc::kEngineStateLayoutVersion,
                             ipc::Access::ReadOnly);
        if (!region) {
            return std::unexpected{std::string{"not an engine-state segment"}};
        }
        auto* const state = region->at<ipc::EngineState>(ipc::kSegmentHeaderSize);
        if (state == nullptr) {
            return std::unexpected{std::string{"too small for the engine's state"}};
        }
        return EngineView{std::move(*region), *state};
    }

    /// The last status block published, or nothing if none was, or it
    /// couldn't be read.
    [[nodiscard]] std::optional<ipc::EngineStatus> status() const {
        const std::optional<ipc::EngineStatus> status =
            ipc::SeqlockReader<ipc::EngineStatus>{state_->status}.try_read(1'000);
        if (!status || status->heartbeat == 0) {
            return std::nullopt;
        }
        return status;
    }

    /// The status block's heartbeat; 0 if there's none yet.
    [[nodiscard]] std::uint64_t heartbeat() const {
        const std::optional<ipc::EngineStatus> current = status();
        return current ? current->heartbeat : 0;
    }

    /// The last mixer block published, or nothing if none was, or it
    /// couldn't be read.
    [[nodiscard]] std::optional<ipc::MixerState> mixer() const {
        const std::optional<ipc::MixerState> mixer =
            ipc::SeqlockReader<ipc::MixerState>{state_->mixer}.try_read(1'000);
        if (!mixer || mixer->heartbeat == 0) {
            return std::nullopt;
        }
        return mixer;
    }

    /// Client slot `slot` as the last status block reports it; all zero if
    /// there's no status block yet, or `slot` is out of range.
    [[nodiscard]] ipc::ClientAck ack(std::size_t slot) const {
        const std::optional<ipc::EngineStatus> current = status();
        if (!current || slot >= ipc::kStateClients) {
            return ipc::ClientAck{};
        }
        return std::span{current->clients}[slot];
    }

    /// The mixer's crossfader position; NaN if there's no mixer block yet.
    [[nodiscard]] double crossfader() const {
        const std::optional<ipc::MixerState> current = mixer();
        return current ? std::bit_cast<double>(current->crossfader)
                       : std::numeric_limits<double>::quiet_NaN();
    }

private:
    EngineView(ipc::Region region, ipc::EngineState& state) noexcept
        : region_{std::move(region)}, state_{&state} {}

    ipc::Region region_;
    ipc::EngineState* state_;
};

/// True if slot `slot` is attached in the last status block.
inline bool attached(const EngineView& view, std::size_t slot) {
    return (view.ack(slot).flags & ipc::kClientAttached) != 0;
}

/// True if, within kApplyWait, the mixer block satisfies `done`, and client
/// slot `slot` is attached and has read up to sequence number `sequence`:
/// the spec's "applied".
template <typename Done>
bool applied(const EngineView& view, std::size_t slot, std::uint64_t sequence, Done done) {
    return eventually(
        [&] {
            const std::optional<ipc::MixerState> mixer = view.mixer();
            const ipc::ClientAck ack = view.ack(slot);
            return mixer && done(*mixer) && (ack.flags & ipc::kClientAttached) != 0 &&
                   ack.last_sequence == sequence;
        },
        kApplyWait);
}

/// applied() for a SetCrossfader to `position` with `sequence`.
inline bool crossfader_applied(const EngineView& view, std::size_t slot, double position,
                               std::uint64_t sequence) {
    return applied(view, slot, sequence, [position](const ipc::MixerState& mixer) {
        return mixer.crossfader == real_code(position);
    });
}

/// True if a first heartbeat appears within 5 s, and then the heartbeat
/// increases between each check, 100 ms apart, for `duration`.
inline bool keeps_beating(const EngineView& view, Milliseconds duration = Milliseconds{500}) {
    std::uint64_t last = 0;
    if (!eventually([&] {
            last = view.heartbeat();
            return last != 0;
        })) {
        return false;
    }
    for (Milliseconds waited{0}; waited < duration; waited += Milliseconds{100}) {
        std::this_thread::sleep_for(Milliseconds{100});
        const std::uint64_t now = view.heartbeat();
        if (now <= last) {
            return false;
        }
        last = now;
    }
    return true;
}

// ---- A client's ring ----------------------------------------------------------

/// A command-ring segment, mapped read-write, with a producer attached: what
/// a client writes commands through (spec requirement 14).
class ClientRing {
public:
    /// Maps `fd` as a command-ring segment, or says why it couldn't.
    static std::expected<ClientRing, std::string> map(int fd) {
        std::expected<ipc::Region, ipc::MapFailure> region =
            ipc::Region::map(fd, ipc::SegmentKind::CommandRing, ipc::kCommandRingLayoutVersion,
                             ipc::Access::ReadWrite);
        if (!region) {
            return std::unexpected{std::string{"not a command-ring segment"}};
        }
        auto* const ring = region->at<ipc::CommandRing>(ipc::kSegmentHeaderSize);
        if (ring == nullptr) {
            return std::unexpected{std::string{"too small for a command ring"}};
        }
        return ClientRing{std::move(*region), *ring};
    }

    /// Writes `slot` into the ring. False if the ring is full or corrupt.
    [[nodiscard]] bool push(const ipc::CommandSlot& slot) noexcept {
        return producer_.try_push(slot);
    }

    /// The ring itself, for tests that scribble on it.
    [[nodiscard]] ipc::CommandRing& ring() const noexcept { return *ring_; }

private:
    ClientRing(ipc::Region region, ipc::CommandRing& ring) noexcept
        : region_{std::move(region)}, ring_{&ring}, producer_{ring} {}

    ipc::Region region_;
    ipc::CommandRing* ring_;
    ipc::RingProducer<ipc::CommandSlot, ipc::kCommandRingCapacity> producer_;
};

/// The slot that `received` names, if it's a RingOpened or an Attached.
inline std::optional<std::uint32_t> slot_of(const std::optional<Received>& received) {
    if (!received) {
        return std::nullopt;
    }
    if (const auto* const opened = std::get_if<ipc::msg::RingOpened>(&received->message.body)) {
        return opened->slot;
    }
    if (const auto* const attached = std::get_if<ipc::msg::Attached>(&received->message.body)) {
        return attached->slot;
    }
    return std::nullopt;
}

/// True if `received` is an Error with `code` and request ID `id`.
inline bool is_error(const std::optional<Received>& received, ipc::ErrorCode code,
                     std::uint32_t id) {
    return processes::test::is<ipc::msg::Error>(received, id) &&
           processes::test::error_code(received) == code;
}

// ---- ks-engine, with the test as its supervisor ---------------------------------

/// The output options these tests give ks-engine.
inline std::vector<std::string> output_options() {
    return {"--backend", "null", "--rate", "44100", "--channels", "2"};
}

/// What a supervisor would hold for a running engine: the runtime
/// directory, the segment factory, the state segment, the socket, and the
/// process. The engine is killed, if it's still running, when this is
/// destroyed.
struct Supervised {
    ipc::test::TempDir dir;
    std::unique_ptr<ipc::SegmentFactory> factory;
    ipc::Fd state_fd;
    /// The supervisor's end of the socket pair, non-blocking.
    std::unique_ptr<ipc::Connection> socket;
    /// The state segment, mapped read-only.
    std::unique_ptr<EngineView> state;
    Process engine;
};

/// Starts ks-engine as kool_spin would, on the null backend, and waits for
/// its Ready.
inline std::unique_ptr<Supervised> start_engine() {
    auto supervised = std::make_unique<Supervised>();
    REQUIRE(!supervised->dir.path().empty());
    std::expected<ipc::SegmentFactory, std::error_code> factory =
        ipc::SegmentFactory::open(supervised->dir.path());
    REQUIRE(factory.has_value());
    supervised->factory = std::make_unique<ipc::SegmentFactory>(std::move(*factory));
    std::expected<ipc::Fd, std::error_code> state =
        supervised->factory->create(ipc::SegmentKind::EngineState, ipc::kEngineStateLayoutVersion,
                                    ipc::kEngineStateSegmentSize);
    REQUIRE(state.has_value());
    supervised->state_fd = std::move(*state);
    std::expected<EngineView, std::string> view = EngineView::map(supervised->state_fd.get());
    REQUIRE(view.has_value());
    supervised->state = std::make_unique<EngineView>(std::move(*view));

    std::expected<std::pair<ipc::Fd, ipc::Fd>, std::error_code> pair = ipc::socket_pair();
    REQUIRE(pair.has_value());
    std::vector<std::string> args{"--supervisor-fd", "3",
                                  "--state-fd",      "4",
                                  "--runtime-dir",   supervised->dir.path().string()};
    const std::vector<std::string> options = output_options();
    args.insert(args.end(), options.begin(), options.end());
    const std::vector<supervisor::Inherit> fds{{.parent = pair->second.get(), .child = 3},
                                               {.parent = supervised->state_fd.get(), .child = 4}};
    supervised->engine = processes::test::start(processes::test::kEnginePath, args, fds);
    REQUIRE(supervised->engine.pid() > 0);
    // Only the engine holds its end now, so the test sees it close.
    pair->second.reset();
    REQUIRE(ipc::test::set_nonblocking(pair->first.get()));
    supervised->socket = std::make_unique<ipc::Connection>(std::move(pair->first));
    REQUIRE(processes::test::is<ipc::msg::Ready>(processes::test::receive(*supervised->socket), 0));
    return supervised;
}

/// Creates a segment in `supervised`'s runtime directory: by default, a
/// command-ring segment as spec requirement 1 says.
inline ipc::Fd create_segment(Supervised& supervised,
                              ipc::SegmentKind kind = ipc::SegmentKind::CommandRing,
                              std::uint32_t version = ipc::kCommandRingLayoutVersion,
                              std::size_t size = ipc::kCommandRingSegmentSize) {
    std::expected<ipc::Fd, std::error_code> segment =
        supervised.factory->create(kind, version, size);
    REQUIRE(segment.has_value());
    return std::move(*segment);
}

/// Sends the engine an Attach for `segment`, with request ID `id`, and
/// returns its answer.
inline std::optional<Received> attach(Supervised& supervised, const ipc::Fd& segment,
                                      std::uint32_t id) {
    const std::array<int, 1> fds{segment.get()};
    if (!processes::test::send(*supervised.socket,
                               ipc::Message{.request_id = id, .body = ipc::msg::Attach{}}, fds)) {
        return std::nullopt;
    }
    return processes::test::receive(*supervised.socket);
}

/// Sends the engine a Detach for `slot`, with request ID `id`, and returns
/// its answer.
inline std::optional<Received> detach(Supervised& supervised, std::uint32_t slot,
                                      std::uint32_t id) {
    if (!processes::test::send(
            *supervised.socket,
            ipc::Message{.request_id = id, .body = ipc::msg::Detach{.slot = slot}})) {
        return std::nullopt;
    }
    return processes::test::receive(*supervised.socket);
}

// ---- kool_spin, with clients ------------------------------------------------------

/// kool_spin in a runtime directory of its own, with ks-engine on the null
/// backend. Whatever happens, kool_spin and any engine left without it are
/// killed when this is destroyed.
class KoolSpin {
public:
    /// Starts kool_spin and waits up to 5 s for supervisor.sock. Its logs go
    /// in the runtime directory, never in the real log folder
    /// (specs/logs/spec.md decision 8).
    KoolSpin() {
        REQUIRE(!dir_.path().empty());
        std::vector<std::string> args{"--runtime-dir",
                                      dir_.path().string(),
                                      "--log-dir",
                                      (dir_.path() / "logs").string(),
                                      "--engine",
                                      std::string{processes::test::kEnginePath},
                                      "--"};
        const std::vector<std::string> options = processes::test::null_backend();
        args.insert(args.end(), options.begin(), options.end());
        process_ = processes::test::start(processes::test::kKoolSpinPath, args);
        REQUIRE(process_.pid() > 0);
        const std::filesystem::path socket = dir_.path() / "supervisor.sock";
        static_cast<void>(
            eventually([&] { return std::filesystem::exists(socket) || !process_.running(); }));
        REQUIRE(std::filesystem::exists(socket));
        REQUIRE(process_.running());
    }

    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_.path(); }
    [[nodiscard]] Process& process() noexcept { return process_; }

    /// The engine's process ID, from its lock; 0 if none is running.
    [[nodiscard]] pid_t engine() const {
        return processes::test::lock_holder(dir_.path(), ipc::kEngineLockName).value_or(0);
    }

private:
    ipc::test::TempDir dir_;
    processes::test::EngineReaper reaper_{dir_.path()};
    Process process_;
};

/// A client that has said Hello: its connection, and the engine's state.
struct Client {
    ipc::Connection connection;
    EngineView state;
};

/// Connects to the supervisor of `dir` and says Hello as `name`, with
/// request ID 1, or says why that failed.
inline std::expected<Client, std::string> join(const std::filesystem::path& dir, std::string name) {
    std::expected<ipc::Connection, std::error_code> connection = processes::test::connect(dir);
    if (!connection) {
        return std::unexpected{"couldn't connect: " + connection.error().message()};
    }
    if (!processes::test::send(
            *connection, ipc::Message{.request_id = 1, .body = ipc::msg::Hello{std::move(name)}})) {
        return std::unexpected{std::string{"couldn't send Hello"}};
    }
    const std::optional<Received> welcome = processes::test::receive(*connection);
    if (!processes::test::is<ipc::msg::Welcome>(welcome, 1) || !welcome ||
        welcome->fds.size() != 1) {
        return std::unexpected{std::string{"no Welcome with one descriptor"}};
    }
    std::expected<EngineView, std::string> view = EngineView::map(welcome->fds.front().get());
    if (!view) {
        return std::unexpected{view.error()};
    }
    return Client{.connection = std::move(*connection), .state = std::move(*view)};
}

/// A ring that RingOpened gave: its slot, and the ring, mapped.
struct OpenedRing {
    std::uint32_t slot;
    ClientRing ring;
};

/// Why open_ring() got no ring: the Error's code, or nothing if the answer
/// was something else, or never came, or the descriptor didn't map.
using OpenFailure = std::optional<ipc::ErrorCode>;

/// Sends OpenRing with request ID `id`, and waits for the answer.
inline std::expected<OpenedRing, OpenFailure> open_ring(ipc::Connection& connection,
                                                        std::uint32_t id) {
    if (!processes::test::send(connection,
                               ipc::Message{.request_id = id, .body = ipc::msg::OpenRing{}})) {
        return std::unexpected{OpenFailure{}};
    }
    std::optional<Received> answer = processes::test::receive(connection);
    // A DecodeEnded (specs/ks-play/spec.md requirement 8) is a notification
    // about an earlier load, not the answer.
    while (answer && std::holds_alternative<ipc::msg::DecodeEnded>(answer->message.body)) {
        answer = processes::test::receive(connection);
    }
    if (!answer || answer->message.request_id != id) {
        return std::unexpected{OpenFailure{}};
    }
    if (const auto* const error = std::get_if<ipc::msg::Error>(&answer->message.body)) {
        return std::unexpected{OpenFailure{error->code}};
    }
    const auto* const opened = std::get_if<ipc::msg::RingOpened>(&answer->message.body);
    if (opened == nullptr || answer->fds.size() != 1) {
        return std::unexpected{OpenFailure{}};
    }
    std::expected<ClientRing, std::string> ring = ClientRing::map(answer->fds.front().get());
    if (!ring) {
        return std::unexpected{OpenFailure{}};
    }
    return OpenedRing{.slot = opened->slot, .ring = std::move(*ring)};
}

/// open_ring(), asking again every 20 ms while the answer is Unavailable,
/// for up to kApplyWait. Each request takes the next ID from `next_id`.
inline std::expected<OpenedRing, OpenFailure> open_ring_retrying(ipc::Connection& connection,
                                                                 std::uint32_t& next_id) {
    const Clock::time_point deadline = Clock::now() + kApplyWait;
    for (;;) {
        std::expected<OpenedRing, OpenFailure> opened = open_ring(connection, next_id++);
        if (opened || opened.error() != OpenFailure{ipc::ErrorCode::Unavailable} ||
            Clock::now() >= deadline) {
            return opened;
        }
        std::this_thread::sleep_for(Milliseconds{20});
    }
}

} // namespace ks::client_rings::test
