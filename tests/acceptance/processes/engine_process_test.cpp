// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/processes/spec.md: the engine process
// (requirements 8 to 12). The test plays the supervisor: it creates the
// runtime directory, the state segment, and a socket pair, and starts
// ks-engine as kool_spin would.

#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/fd_passing.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/region.h>
#include <ks/ipc/runtime_dir.h>
#include <ks/ipc/segment.h>
#include <ks/ipc/state.h>
#include <ks/supervisor/spawn.h>

#include "processes_support.h"
#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <unistd.h>

#include <csignal>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using ks::ipc::Connection;
using ks::ipc::ErrorCode;
using ks::ipc::Fd;
using ks::ipc::kEngineLockName;
using ks::ipc::kEngineStateLayoutVersion;
using ks::ipc::kEngineStateSegmentSize;
using ks::ipc::LockFile;
using ks::ipc::Message;
using ks::ipc::SegmentFactory;
using ks::ipc::SegmentKind;
using ks::ipc::send_some;
using ks::ipc::socket_pair;
using ks::ipc::test::set_nonblocking;
using ks::ipc::test::TempDir;
using ks::processes::test::error_code;
using ks::processes::test::heartbeat_keeps_increasing;
using ks::processes::test::is;
using ks::processes::test::kEnginePath;
using ks::processes::test::lock_holder;
using ks::processes::test::pings;
using ks::processes::test::Process;
using ks::processes::test::receive;
using ks::processes::test::Received;
using ks::processes::test::send;
using ks::processes::test::silenced;
using ks::processes::test::start;
using ks::processes::test::StateView;
using ks::supervisor::Inherit;
namespace msg = ks::ipc::msg;

/// The output options these tests give ks-engine.
std::vector<std::string> output_options() {
    return {"--backend", "null", "--rate", "44100", "--channels", "2"};
}

/// What a supervisor would hold for a running engine: the runtime
/// directory, the state segment, the socket, and the process.
struct Supervised {
    TempDir dir;
    std::unique_ptr<SegmentFactory> factory;
    Fd state_fd;
    /// The supervisor's end of the socket pair, non-blocking.
    std::unique_ptr<Connection> socket;
    /// The state segment, mapped read-only.
    std::unique_ptr<StateView> state;
    Process engine;
};

/// Creates the runtime directory's factory and the state segment.
std::unique_ptr<Supervised> prepare() {
    auto supervised = std::make_unique<Supervised>();
    REQUIRE(!supervised->dir.path().empty());
    std::expected<SegmentFactory, std::error_code> factory =
        SegmentFactory::open(supervised->dir.path());
    REQUIRE(factory.has_value());
    supervised->factory = std::make_unique<SegmentFactory>(std::move(*factory));
    std::expected<Fd, std::error_code> state = supervised->factory->create(
        SegmentKind::EngineState, kEngineStateLayoutVersion, kEngineStateSegmentSize);
    REQUIRE(state.has_value());
    supervised->state_fd = std::move(*state);
    std::expected<StateView, std::string> view = StateView::map(supervised->state_fd.get());
    REQUIRE(view.has_value());
    supervised->state = std::make_unique<StateView>(std::move(*view));
    return supervised;
}

/// The arguments kool_spin gives ks-engine, then `options`.
std::vector<std::string> engine_args(const Supervised& supervised,
                                     const std::vector<std::string>& options) {
    std::vector<std::string> args{
        "--supervisor-fd", "3", "--state-fd", "4", "--runtime-dir", supervised.dir.path().string()};
    args.insert(args.end(), options.begin(), options.end());
    return args;
}

/// Starts ks-engine as kool_spin would, with `options` as the output
/// options.
std::unique_ptr<Supervised>
start_engine(const std::vector<std::string>& options = output_options()) {
    std::unique_ptr<Supervised> supervised = prepare();
    std::expected<std::pair<Fd, Fd>, std::error_code> pair = socket_pair();
    REQUIRE(pair.has_value());
    const std::vector<Inherit> fds{{.parent = pair->second.get(), .child = 3},
                                   {.parent = supervised->state_fd.get(), .child = 4}};
    supervised->engine = start(kEnginePath, engine_args(*supervised, options), fds);
    REQUIRE(supervised->engine.pid() > 0);
    // Only the engine holds its end now, so the test sees it close.
    pair->second.reset();
    REQUIRE(set_nonblocking(pair->first.get()));
    supervised->socket = std::make_unique<Connection>(std::move(pair->first));
    return supervised;
}

/// True if the engine's first message is Ready, with request ID 0.
bool ready(Supervised& supervised) {
    return is<msg::Ready>(receive(*supervised.socket), 0);
}

/// How ks-engine exits with `args` and `fds`, or nothing if it didn't start
/// or didn't exit within 5 s.
std::optional<int> exit_status(const std::vector<std::string>& args,
                               const std::vector<Inherit>& fds) {
    Process engine = start(kEnginePath, args, fds);
    if (engine.pid() <= 0) {
        return std::nullopt;
    }
    return engine.wait();
}

} // namespace

TEST_CASE("Engine process: starts and reports Ready", "[engine-process]") {
    const std::unique_ptr<Supervised> engine = start_engine();
    REQUIRE(ready(*engine));

    CHECK(heartbeat_keeps_increasing(*engine->state));
    const auto status = engine->state->status();
    REQUIRE(status.has_value());
    CHECK(status.value_or(ks::ipc::EngineStatus{}).output_state == 1); // playing
    CHECK(status.value_or(ks::ipc::EngineStatus{}).sample_rate == 44'100);
    CHECK(lock_holder(engine->dir.path(), kEngineLockName) == engine->engine.pid());
    CHECK(pings(*engine->socket, 7));
}

TEST_CASE("Engine process: Stop fades out and exits", "[engine-process]") {
    const std::unique_ptr<Supervised> engine = start_engine();
    REQUIRE(ready(*engine));

    REQUIRE(send(*engine->socket, Message{.request_id = 9, .body = msg::Stop{}}));
    CHECK(is<msg::Done>(receive(*engine->socket), 9));
    CHECK(engine->engine.wait() == 0);
    CHECK(silenced(*engine->state));
    CHECK_FALSE(lock_holder(engine->dir.path(), kEngineLockName).has_value());
}

TEST_CASE("Engine process: signals fade out and exit", "[engine-process]") {
    int signal = 0;
    SECTION("SIGTERM") {
        signal = SIGTERM;
    }
    SECTION("SIGINT") {
        signal = SIGINT;
    }
    SECTION("SIGHUP") {
        signal = SIGHUP;
    }
    CAPTURE(signal);

    const std::unique_ptr<Supervised> engine = start_engine();
    REQUIRE(ready(*engine));
    engine->engine.signal(signal);
    CHECK(engine->engine.wait() == 0);
    CHECK(silenced(*engine->state));
    CHECK_FALSE(lock_holder(engine->dir.path(), kEngineLockName).has_value());
}

TEST_CASE("Engine process: keeps playing without its supervisor", "[engine-process]") {
    const std::unique_ptr<Supervised> engine = start_engine();
    REQUIRE(ready(*engine));

    engine->socket.reset();
    CHECK(heartbeat_keeps_increasing(*engine->state));
    CHECK(engine->engine.running());

    engine->engine.signal(SIGTERM);
    CHECK(engine->engine.wait() == 0);
    CHECK(silenced(*engine->state));
}

TEST_CASE("Engine process: what it refuses from its supervisor", "[engine-process]") {
    const std::unique_ptr<Supervised> engine = start_engine();
    REQUIRE(ready(*engine));

    // A valid message it doesn't take: NotSupported, and it carries on.
    REQUIRE(send(*engine->socket, Message{.request_id = 3, .body = msg::Hello{"test"}}));
    const std::optional<Received> refused = receive(*engine->socket);
    CHECK(error_code(refused) == ErrorCode::NotSupported);
    CHECK((refused && refused->message.request_id == 3));
    CHECK(pings(*engine->socket, 4));

    // A bad header: BadMessage, then the socket closes, and it plays on.
    REQUIRE(send_some(engine->socket->socket(), ks::processes::test::bad_header(), {}).has_value());
    const std::optional<Received> bad = receive(*engine->socket);
    CHECK(error_code(bad) == ErrorCode::BadMessage);
    CHECK((bad && bad->message.request_id == 0));
    CHECK(ks::processes::test::closes(*engine->socket));
    CHECK(heartbeat_keeps_increasing(*engine->state));
    CHECK(engine->engine.running());
}

TEST_CASE("Engine process: start failures", "[engine-process]") {
    const std::unique_ptr<Supervised> supervised = prepare();
    std::expected<std::pair<Fd, Fd>, std::error_code> pair = socket_pair();
    REQUIRE(pair.has_value());
    const std::vector<Inherit> fds{{.parent = pair->second.get(), .child = 3},
                                   {.parent = supervised->state_fd.get(), .child = 4}};
    const std::vector<std::string> args = engine_args(*supervised, output_options());

    SECTION("usage errors exit with 2") {
        std::vector<std::string> no_state_fd = args;
        no_state_fd.erase(no_state_fd.begin() + 2, no_state_fd.begin() + 4);
        CHECK(exit_status(no_state_fd, fds) == 2);

        std::vector<std::string> unknown = args;
        unknown.emplace_back("--bogus");
        CHECK(exit_status(unknown, fds) == 2);

        const std::vector<std::string> bad_rate =
            engine_args(*supervised, {"--backend", "null", "--rate", "5"});
        CHECK(exit_status(bad_rate, fds) == 2);

        // The null backend lists one device, number 0.
        const std::vector<std::string> unlisted =
            engine_args(*supervised, {"--backend", "null", "--device", "1"});
        CHECK(exit_status(unlisted, fds) == 2);
    }

    SECTION("an engine lock already held") {
        const std::expected<LockFile, LockFile::Failure> lock =
            LockFile::acquire(supervised->dir.path(), kEngineLockName);
        REQUIRE(lock.has_value());
        CHECK(exit_status(args, fds) == 1);
    }

    SECTION("a state segment of the wrong kind") {
        std::expected<Fd, std::error_code> ring =
            supervised->factory->create(SegmentKind::CommandRing, 1, 4096);
        REQUIRE(ring.has_value());
        const std::vector<Inherit> wrong{{.parent = pair->second.get(), .child = 3},
                                         {.parent = ring->get(), .child = 4}};
        CHECK(exit_status(args, wrong) == 1);
    }

    SECTION("a supervisor descriptor that isn't a socket") {
        const std::string path = (supervised->dir.path() / "file").string();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open() is variadic
        const Fd file{::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
        REQUIRE(file.valid());
        const std::vector<Inherit> wrong{{.parent = file.get(), .child = 3},
                                         {.parent = supervised->state_fd.get(), .child = 4}};
        CHECK(exit_status(args, wrong) == 1);
    }

    SECTION("a runtime directory that doesn't exist") {
        std::vector<std::string> missing = args;
        missing.at(5) = (supervised->dir.path() / "missing").string();
        CHECK(exit_status(missing, fds) == 1);
    }

    // None of them took the lock and kept it.
    CHECK_FALSE(lock_holder(supervised->dir.path(), kEngineLockName).value_or(0) > 0);
}
