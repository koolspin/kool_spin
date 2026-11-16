// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/logs/spec.md: ks-engine logging through its
// segment (requirements 16 to 19). The test plays the supervisor, as in the
// processes tests: it creates the segments, and starts ks-engine as
// kool_spin would, with its standard error in a file.

#include <ks/ipc/command.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/layout.h>
#include <ks/ipc/log.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/runtime_dir.h>

#include "logs_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ks::client_rings::test::is_error;
using ks::client_rings::test::keeps_beating;
using ks::client_rings::test::slot_of;
using ks::ipc::ErrorCode;
using ks::ipc::LogLevel;
using ks::ipc::LogReader;
using ks::ipc::LogRecord;
using ks::logs::test::LoggedEngine;
using ks::logs::test::read_lines;
using ks::logs::test::steady_now;
using ks::logs::test::text_of;
using ks::processes::test::eventually;
using ks::processes::test::is;
using ks::processes::test::Milliseconds;
using ks::processes::test::pings;
namespace msg = ks::ipc::msg;

/// The records read so far from an engine's log.
class Records {
public:
    explicit Records(ks::ipc::LogSegment& segment) : reader_{segment} {}

    /// Reads whatever is in the ring.
    void read() {
        while (const std::optional<LogRecord> record = reader_.next()) {
            records_.push_back(*record);
        }
    }

    /// Waits up to 2 s for a record at `level` whose text contains `phrase`.
    [[nodiscard]] bool wait_for(LogLevel level, std::string_view phrase) {
        return eventually(
            [&] {
                read();
                return has(level, phrase);
            },
            ks::logs::test::kLineWait);
    }

    [[nodiscard]] bool has(LogLevel level, std::string_view phrase) const {
        return std::ranges::any_of(records_, [&](const LogRecord& record) {
            return record.level == static_cast<std::uint64_t>(level) &&
                   text_of(record).find(phrase) != std::string::npos;
        });
    }

    [[nodiscard]] const std::vector<LogRecord>& all() const noexcept { return records_; }

private:
    LogReader reader_;
    std::vector<LogRecord> records_;
};

/// ks-engine's exit status with `args`, or nothing if it's still running
/// after 5 s; it's killed then.
std::optional<int> exit_status(LoggedEngine& engine, const std::vector<std::string>& args) {
    const std::unique_ptr<ks::ipc::Connection> socket = ks::logs::test::launch_engine(engine, args);
    return engine.supervised->engine.wait();
}

} // namespace

TEST_CASE("Engine logs: through its segment", "[engine]") {
    const std::uint64_t before = steady_now();
    const std::unique_ptr<LoggedEngine> engine = ks::logs::test::start_logged_engine();
    ks::client_rings::test::Supervised& supervised = *engine->supervised;
    Records records{*engine->log->segment};

    CHECK(records.wait_for(LogLevel::Info, "ready"));
    CHECK(records.wait_for(LogLevel::Info, "output playing"));

    const ks::ipc::Fd ring = ks::client_rings::test::create_segment(supervised);
    const std::optional<ks::processes::test::Received> answer =
        ks::client_rings::test::attach(supervised, ring, 7);
    REQUIRE(is<msg::Attached>(answer, 7));
    CHECK(slot_of(answer) == 0U);
    CHECK(records.wait_for(LogLevel::Info, "attached a ring in slot 0"));
    const std::uint64_t after = steady_now();
    for (const LogRecord& record : records.all()) {
        CAPTURE(text_of(record));
        CHECK(record.time >= before);
        CHECK(record.time <= after);
    }

    supervised.engine.signal(SIGTERM);
    CHECK(supervised.engine.wait() == 0);
    records.read();
    REQUIRE(!records.all().empty());
    const LogRecord& last = records.all().back();
    CHECK(last.level == static_cast<std::uint64_t>(LogLevel::Info));
    CHECK(text_of(last).find("stopped") != std::string::npos);
    CHECK(std::filesystem::exists(engine->err));
    CHECK(read_lines(engine->err).empty());
}

TEST_CASE("Engine logs: a bad log descriptor", "[engine]") {
    const std::unique_ptr<LoggedEngine> engine = ks::logs::test::prepare_engine();
    const std::filesystem::path lock =
        engine->supervised->dir.path() / (std::string{ks::ipc::kEngineLockName} + ".lock");

    SECTION("a closed descriptor") {
        CHECK(exit_status(*engine, ks::logs::test::engine_args(*engine, {"--log-fd", "9"})) == 1);
        CHECK(!std::filesystem::exists(lock));
        CHECK(!read_lines(engine->err).empty());
    }

    SECTION("not a log segment") {
        // Descriptor 4 is the engine-state segment.
        CHECK(exit_status(*engine, ks::logs::test::engine_args(*engine, {"--log-fd", "4"})) == 1);
        CHECK(!std::filesystem::exists(lock));
        CHECK(!read_lines(engine->err).empty());
    }

    SECTION("given twice") {
        CHECK(exit_status(*engine, ks::logs::test::engine_args(
                                       *engine, {"--log-fd", "5", "--log-fd", "5"})) == 2);
    }
}

TEST_CASE("Engine logs: a full ring never blocks it", "[engine]") {
    const std::unique_ptr<LoggedEngine> engine = ks::logs::test::start_logged_engine();
    ks::client_rings::test::Supervised& supervised = *engine->supervised;

    // Nothing reads the log. Each refused Detach is a warning line.
    constexpr std::uint32_t kDetaches = 1'100;
    for (std::uint32_t i = 0; i < kDetaches; ++i) {
        const std::uint32_t id = 100 + i;
        CAPTURE(i);
        REQUIRE(is_error(ks::client_rings::test::detach(supervised, 5, id), ErrorCode::Unavailable,
                         id));
    }
    CHECK(keeps_beating(*supervised.state));
    CHECK(pings(*supervised.socket, 50));

    ks::ipc::LogSegment& log = *engine->log->segment;
    CHECK(ks::ipc::load_shared(log.dropped, std::memory_order_acquire) > 0);
    const std::uint64_t head = ks::ipc::load_shared(log.ring.head, std::memory_order_acquire);
    const std::uint64_t tail = ks::ipc::load_shared(log.ring.tail, std::memory_order_acquire);
    CHECK(head - tail == ks::ipc::kLogRingCapacity);
}
