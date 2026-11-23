// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/processes/spec.md: the supervisor (requirements
// 13 to 22). Each test runs kool_spin in its own runtime directory, with
// ks-engine on miniaudio's null backend, or the probe standing in for it.

#include <ks/ipc/connection.h>
#include <ks/ipc/fd_passing.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/runtime_dir.h>
#include <ks/supervisor/run.h>

#include "processes_support.h"
#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using ks::ipc::Connection;
using ks::ipc::ErrorCode;
using ks::ipc::kEngineLockName;
using ks::ipc::kSupervisorLockName;
using ks::ipc::LockFile;
using ks::ipc::Message;
using ks::ipc::send_some;
using ks::ipc::test::TempDir;
using ks::processes::test::bad_header;
using ks::processes::test::Clock;
using ks::processes::test::closes;
using ks::processes::test::connect;
using ks::processes::test::EngineReaper;
using ks::processes::test::error_code;
using ks::processes::test::eventually;
using ks::processes::test::heartbeat_keeps_increasing;
using ks::processes::test::hello;
using ks::processes::test::is;
using ks::processes::test::kEnginePath;
using ks::processes::test::kKoolSpinPath;
using ks::processes::test::kProbePath;
using ks::processes::test::lock_holder;
using ks::processes::test::null_backend;
using ks::processes::test::pings;
using ks::processes::test::Process;
using ks::processes::test::read_until_closed;
using ks::processes::test::receive;
using ks::processes::test::Received;
using ks::processes::test::send;
using ks::processes::test::silenced;
using ks::processes::test::start;
using ks::processes::test::StateView;
using ks::processes::test::wait_for_report;
using ks::supervisor::kEngineReadyTimeout;
using ks::supervisor::kEngineStopTimeout;
using ks::supervisor::kMaxConnections;
namespace msg = ks::ipc::msg;

/// kool_spin in a runtime directory of its own. Whatever happens, kool_spin
/// and any engine left without it are killed when this is destroyed.
class Supervisor {
public:
    Supervisor() { REQUIRE(!dir_.path().empty()); }

    /// Starts kool_spin with `engine` as the engine, and `engine_args`
    /// after "--". Its logs go in the runtime directory, never in the real
    /// log folder (specs/logs/spec.md decision 8).
    void start(std::string_view engine = kEnginePath,
               const std::vector<std::string>& engine_args = null_backend()) {
        std::vector<std::string> args{
            "--runtime-dir", dir_.path().string(), "--log-dir", (dir_.path() / "logs").string(),
            "--engine",      std::string{engine},  "--"};
        args.insert(args.end(), engine_args.begin(), engine_args.end());
        process_ = ks::processes::test::start(kKoolSpinPath, args);
        REQUIRE(process_.pid() > 0);
    }

    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_.path(); }
    [[nodiscard]] std::filesystem::path socket() const { return dir_.path() / "supervisor.sock"; }
    [[nodiscard]] Process& process() { return process_; }

    /// Waits up to 5 s for supervisor.sock to appear, while kool_spin runs.
    [[nodiscard]] bool listening() {
        static_cast<void>(eventually(
            [this] { return std::filesystem::exists(socket()) || !process_.running(); }));
        return std::filesystem::exists(socket()) && process_.running();
    }

    /// The engine's process ID, from its lock; 0 if none is running.
    [[nodiscard]] pid_t engine() const {
        return lock_holder(dir_.path(), kEngineLockName).value_or(0);
    }

    /// True if neither lock is held.
    [[nodiscard]] bool locks_free() const {
        return !lock_holder(dir_.path(), kSupervisorLockName) &&
               !lock_holder(dir_.path(), kEngineLockName);
    }

private:
    TempDir dir_;
    EngineReaper reaper_{dir_.path()};
    Process process_;
};

/// A client that has said Hello: its connection, and the engine's state.
struct Client {
    Connection connection;
    StateView state;
};

/// Connects to the supervisor of `dir` and says Hello, or says why that
/// failed.
std::expected<Client, std::string> welcomed(const std::filesystem::path& dir,
                                            std::string name = "test") {
    std::expected<Connection, std::error_code> connection = connect(dir);
    if (!connection) {
        return std::unexpected{"couldn't connect: " + connection.error().message()};
    }
    std::expected<StateView, std::string> state = hello(*connection, std::move(name));
    if (!state) {
        return std::unexpected{state.error()};
    }
    return Client{.connection = std::move(*connection), .state = std::move(*state)};
}

/// The arguments for kool_spin on `dir`, with ks-engine on the null backend,
/// and its logs in `dir` (specs/logs/spec.md decision 8).
std::vector<std::string> kool_spin_args(const std::filesystem::path& dir) {
    return {"--runtime-dir",
            dir.string(),
            "--log-dir",
            (dir / "logs").string(),
            "--engine",
            std::string{kEnginePath},
            "--",
            "--backend",
            "null"};
}

} // namespace

TEST_CASE("Supervisor: starts the engine and welcomes clients", "[supervisor]") {
    Supervisor supervisor;
    supervisor.start();
    REQUIRE(supervisor.listening());

    std::expected<Client, std::string> client = welcomed(supervisor.dir());
    REQUIRE(client.has_value());
    CHECK(heartbeat_keeps_increasing(client->state));

    CHECK(lock_holder(supervisor.dir(), kSupervisorLockName) == supervisor.process().pid());
    const pid_t engine = supervisor.engine();
    CHECK(engine > 0);
    CHECK(engine != supervisor.process().pid());
    CHECK(::getpgid(engine) == engine);
    CHECK(pings(client->connection, 2));
}

TEST_CASE("Supervisor: a client's Stop stops everything", "[supervisor]") {
    Supervisor supervisor;
    supervisor.start();
    REQUIRE(supervisor.listening());
    std::expected<Client, std::string> client = welcomed(supervisor.dir());
    REQUIRE(client.has_value());

    REQUIRE(send(client->connection, Message{.request_id = 5, .body = msg::Stop{}}));
    CHECK(is<msg::Done>(receive(client->connection), 5));
    CHECK(silenced(client->state));
    CHECK(supervisor.process().wait() == 0);
    CHECK_FALSE(std::filesystem::exists(supervisor.socket()));
    CHECK(supervisor.locks_free());
}

TEST_CASE("Supervisor: signals stop everything", "[supervisor]") {
    int signal = 0;
    bool to_group = false;
    SECTION("SIGINT") {
        signal = SIGINT;
    }
    SECTION("SIGTERM") {
        signal = SIGTERM;
    }
    SECTION("SIGHUP") {
        signal = SIGHUP;
    }
    SECTION("SIGINT to the process group, as Ctrl-C sends it") {
        signal = SIGINT;
        to_group = true;
    }
    CAPTURE(signal, to_group);

    Supervisor supervisor;
    supervisor.start();
    REQUIRE(supervisor.listening());
    std::expected<Client, std::string> client = welcomed(supervisor.dir());
    REQUIRE(client.has_value());

    if (to_group) {
        REQUIRE(::kill(-supervisor.process().pid(), signal) == 0);
    } else {
        supervisor.process().signal(signal);
    }
    CHECK(supervisor.process().wait() == 0);
    CHECK(silenced(client->state));
    CHECK_FALSE(std::filesystem::exists(supervisor.socket()));
    CHECK(supervisor.locks_free());
}

TEST_CASE("Supervisor: restarts the engine when it ends", "[supervisor]") {
    // specs/engine-restart/spec.md requirement 1: an engine that ends while the
    // supervisor is running is restarted, and clients stay connected.
    Supervisor supervisor;
    supervisor.start();
    REQUIRE(supervisor.listening());
    std::expected<Client, std::string> client = welcomed(supervisor.dir());
    REQUIRE(client.has_value());
    const pid_t engine = supervisor.engine();
    REQUIRE(engine > 0);

    REQUIRE(::kill(engine, SIGKILL) == 0);
    CHECK(eventually([&] {
        const pid_t now = supervisor.engine();
        return now > 0 && now != engine;
    }));
    CHECK(supervisor.process().running());
    CHECK(std::filesystem::exists(supervisor.socket()));
    // The client's connection is still open, and the same state segment shows
    // the new engine's heartbeat.
    CHECK(pings(client->connection, 21));
    CHECK(heartbeat_keeps_increasing(client->state));
}

TEST_CASE("Supervisor: the engine outlives it", "[supervisor]") {
    Supervisor supervisor;
    supervisor.start();
    REQUIRE(supervisor.listening());
    std::expected<Client, std::string> client = welcomed(supervisor.dir());
    REQUIRE(client.has_value());
    const pid_t engine = supervisor.engine();
    REQUIRE(engine > 0);

    supervisor.process().signal(SIGKILL);
    CHECK(supervisor.process().wait() == 128 + SIGKILL);
    CHECK(closes(client->connection));
    CHECK(heartbeat_keeps_increasing(client->state));
    CHECK(supervisor.engine() == engine);

    REQUIRE(::kill(engine, SIGTERM) == 0);
    CHECK(eventually([&] { return supervisor.engine() == 0; }));
    CHECK(silenced(client->state));
}

TEST_CASE("Supervisor: one at a time", "[supervisor]") {
    SECTION("a second supervisor") {
        Supervisor supervisor;
        supervisor.start();
        REQUIRE(supervisor.listening());
        std::expected<Client, std::string> client = welcomed(supervisor.dir());
        REQUIRE(client.has_value());

        Process second = start(kKoolSpinPath, kool_spin_args(supervisor.dir()));
        REQUIRE(second.pid() > 0);
        CHECK(second.wait() == 1);
        CHECK(pings(client->connection, 3));
        CHECK(supervisor.process().running());
    }

    SECTION("an engine still running") {
        Supervisor supervisor;
        const std::expected<LockFile, LockFile::Failure> lock =
            LockFile::acquire(supervisor.dir(), kEngineLockName);
        REQUIRE(lock.has_value());
        // The probe writes a report if it's started at all.
        const std::filesystem::path report = supervisor.dir() / "report";
        supervisor.start(kProbePath, {"--report", report.string()});
        CHECK(supervisor.process().wait() == 1);
        CHECK_FALSE(std::filesystem::exists(report));
        CHECK_FALSE(std::filesystem::exists(supervisor.socket()));
        CHECK_FALSE(lock_holder(supervisor.dir(), kSupervisorLockName).has_value());
    }
}

TEST_CASE("Supervisor: an engine that doesn't start", "[supervisor]") {
    Supervisor supervisor;

    SECTION("a bad engine option") {
        supervisor.start(kEnginePath, {"--backend", "null", "--rate", "5"});
        CHECK(supervisor.process().wait() == 1);
    }

    SECTION("no engine program") {
        supervisor.start((supervisor.dir() / "missing").string());
        CHECK(supervisor.process().wait() == 1);
    }

    SECTION("no Ready") {
        const std::filesystem::path report = supervisor.dir() / "report";
        const Clock::time_point started = Clock::now();
        supervisor.start(kProbePath, {"--mode", "silent", "--report", report.string()});
        const std::optional<int> status =
            supervisor.process().wait(kEngineReadyTimeout + std::chrono::seconds{5});
        const Clock::duration took = Clock::now() - started;
        CHECK(status == 1);
        CHECK(took >= kEngineReadyTimeout - std::chrono::seconds{1});
        const auto probe = wait_for_report(report);
        REQUIRE(probe.has_value());
        CHECK_FALSE(
            ks::processes::test::exists(probe.value_or(ks::processes::test::ProbeReport{}).pid));
    }

    CHECK_FALSE(std::filesystem::exists(supervisor.socket()));
    CHECK(supervisor.locks_free());
}

TEST_CASE("Supervisor: an engine that won't stop is killed", "[supervisor]") {
    Supervisor supervisor;
    const std::filesystem::path report = supervisor.dir() / "report";
    supervisor.start(kProbePath, {"--mode", "ready-stuck", "--report", report.string()});
    REQUIRE(supervisor.listening());
    std::expected<Client, std::string> client = welcomed(supervisor.dir());
    REQUIRE(client.has_value());
    const auto probe = wait_for_report(report);
    REQUIRE(probe.has_value());

    REQUIRE(send(client->connection, Message{.request_id = 4, .body = msg::Stop{}}));
    const Clock::time_point asked = Clock::now();
    const std::optional<int> status =
        supervisor.process().wait(kEngineStopTimeout + std::chrono::seconds{5});
    const Clock::duration took = Clock::now() - asked;
    CHECK(status == 3);
    CHECK(took < kEngineStopTimeout + std::chrono::seconds{3});
    CHECK_FALSE(
        ks::processes::test::exists(probe.value_or(ks::processes::test::ProbeReport{}).pid));
    const ks::processes::test::Closing closing = read_until_closed(client->connection);
    CHECK(closing.closed);
    CHECK_FALSE(closing.saw_done);
}

TEST_CASE("Supervisor: what it refuses from clients", "[supervisor]") {
    Supervisor supervisor;
    supervisor.start();
    REQUIRE(supervisor.listening());

    SECTION("Hello first") {
        std::expected<Connection, std::error_code> connection = connect(supervisor.dir());
        REQUIRE(connection.has_value());
        REQUIRE(send(*connection, Message{.request_id = 5, .body = msg::Ping{}}));
        CHECK(error_code(receive(*connection)) == ErrorCode::BadMessage);
        CHECK(closes(*connection));
    }

    SECTION("a bad header") {
        std::expected<Connection, std::error_code> connection = connect(supervisor.dir());
        REQUIRE(connection.has_value());
        REQUIRE(send_some(connection->socket(), bad_header(), {}).has_value());
        CHECK(closes(*connection));
    }

    SECTION("a second Hello, and a Pong") {
        std::expected<Client, std::string> client = welcomed(supervisor.dir());
        REQUIRE(client.has_value());
        REQUIRE(send(client->connection, Message{.request_id = 2, .body = msg::Hello{"again"}}));
        const std::optional<Received> again = receive(client->connection);
        CHECK(error_code(again) == ErrorCode::NotSupported);
        CHECK((again && again->message.request_id == 2));
        REQUIRE(send(client->connection, Message{.request_id = 3, .body = msg::Pong{}}));
        const std::optional<Received> pong = receive(client->connection);
        CHECK(error_code(pong) == ErrorCode::NotSupported);
        CHECK((pong && pong->message.request_id == 3));
        CHECK(pings(client->connection, 4));
    }

    SECTION("a client leaving") {
        std::expected<Client, std::string> staying = welcomed(supervisor.dir(), "staying");
        REQUIRE(staying.has_value());
        {
            const std::expected<Client, std::string> leaving =
                welcomed(supervisor.dir(), "leaving");
            REQUIRE(leaving.has_value());
        }
        CHECK(pings(staying->connection, 2));
        CHECK(heartbeat_keeps_increasing(staying->state));
    }

    SECTION("the connection limit") {
        std::vector<Client> clients;
        for (std::size_t i = 0; i < kMaxConnections; ++i) {
            CAPTURE(i);
            std::expected<Client, std::string> client = welcomed(supervisor.dir());
            REQUIRE(client.has_value());
            clients.push_back(std::move(*client));
        }
        // One more is closed as soon as it's accepted. Its own user-ID check
        // may already see it closed, so failing to connect also counts.
        std::expected<Connection, std::error_code> extra = connect(supervisor.dir());
        if (extra) {
            CHECK(closes(*extra));
        }
        for (std::size_t i = 0; i < clients.size(); ++i) {
            CAPTURE(i);
            CHECK(pings(clients.at(i).connection, static_cast<std::uint32_t>(i + 1)));
        }
    }

    // Whatever the clients did, the supervisor carries on. It may still be
    // closing their connections, so a new client may have to try again.
    CHECK(supervisor.process().running());
    CHECK(eventually([&] { return welcomed(supervisor.dir(), "after").has_value(); }));
}

TEST_CASE("Supervisor: usage", "[supervisor]") {
    Process unknown = start(kKoolSpinPath, {"--bogus"});
    REQUIRE(unknown.pid() > 0);
    CHECK(unknown.wait() == 2);

    Process help = start(kKoolSpinPath, {"--help"});
    REQUIRE(help.pid() > 0);
    CHECK(help.wait() == 0);
}
