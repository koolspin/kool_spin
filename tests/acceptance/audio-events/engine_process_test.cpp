// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/audio-events/spec.md requirements 8 to 10:
// ks-engine logs the audio thread's events, summarized, and takes its signals
// on the control thread only. The test plays the supervisor, as in the logs
// and client-rings tests: it creates the segments, starts ks-engine as
// kool_spin would, and reads its log segment.

#include <ks/ipc/command.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/layout.h>
#include <ks/ipc/log.h>
#include <ks/ipc/messages.h>

#include "logs_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ks::client_rings::test::ClientRing;
using ks::client_rings::test::Supervised;
using ks::ipc::LogLevel;
using ks::ipc::LogReader;
using ks::ipc::LogRecord;
using ks::logs::test::LoggedEngine;
using ks::logs::test::text_of;
using ks::processes::test::eventually;
using ks::processes::test::is;
using ks::processes::test::Milliseconds;
namespace msg = ks::ipc::msg;

/// The number of commands in a line about slot 0's rejections, or nothing if
/// it isn't one (spec requirement 10).
std::optional<std::uint64_t> rejected_count(const std::string& text) {
    static const std::regex kPattern{R"(rejected (\d+) (more )?commands? from slot 0\b)"};
    std::smatch match;
    if (!std::regex_search(text, match, kPattern)) {
        return std::nullopt;
    }
    return std::stoull(match[1].str());
}

/// The lines about slot 0's rejections in a log.
struct Rejections {
    /// Their counts, added up.
    std::uint64_t total = 0;
    std::size_t lines = 0;
    bool all_warnings = true;
    /// The position in the log of the last of them; 0 if there's none.
    std::size_t last = 0;
};

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
                return std::ranges::any_of(records_, [&](const LogRecord& record) {
                    return record.level == static_cast<std::uint64_t>(level) &&
                           text_of(record).find(phrase) != std::string::npos;
                });
            },
            ks::logs::test::kLineWait);
    }

    /// The lines read so far about slot 0's rejections.
    [[nodiscard]] Rejections rejections() const {
        Rejections found;
        for (std::size_t i = 0; i < records_.size(); ++i) {
            const LogRecord& record = records_.at(i);
            const std::optional<std::uint64_t> count = rejected_count(text_of(record));
            if (!count) {
                continue;
            }
            found.total += *count;
            ++found.lines;
            found.all_warnings =
                found.all_warnings && record.level == static_cast<std::uint64_t>(LogLevel::Warning);
            found.last = i;
        }
        return found;
    }

    /// The position of the first record whose text contains `phrase`.
    [[nodiscard]] std::optional<std::size_t> find(std::string_view phrase) const {
        for (std::size_t i = 0; i < records_.size(); ++i) {
            if (text_of(records_.at(i)).find(phrase) != std::string::npos) {
                return i;
            }
        }
        return std::nullopt;
    }

private:
    LogReader reader_;
    std::vector<LogRecord> records_;
};

/// A client's ring, attached by the test, as the supervisor would.
struct AttachedRing {
    ks::ipc::Fd segment;
    ClientRing ring;
};

/// Creates a ring segment, maps it as a client would, and attaches it. The
/// engine puts it in slot 0.
AttachedRing attach_ring(Supervised& supervised) {
    ks::ipc::Fd segment = ks::client_rings::test::create_segment(supervised);
    std::expected<ClientRing, std::string> ring = ClientRing::map(segment.get());
    REQUIRE(ring.has_value());
    const std::optional<ks::processes::test::Received> answer =
        ks::client_rings::test::attach(supervised, segment, 1);
    REQUIRE(is<msg::Attached>(answer, 1));
    REQUIRE(ks::client_rings::test::slot_of(answer) == 0U);
    return AttachedRing{.segment = std::move(segment), .ring = std::move(*ring)};
}

/// A command of a type that doesn't exist.
ks::ipc::CommandSlot unknown_command(std::uint64_t sequence) {
    return ks::ipc::CommandSlot{.sequence = sequence, .type = 99, .target = 0, .payload = {}};
}

/// Writes commands of unknown type with sequence numbers `first` to `last`,
/// waiting for room as the engine reads them. False if the ring stays full
/// for 2 s.
bool write_unknown(ClientRing& ring, std::uint64_t first, std::uint64_t last) {
    for (std::uint64_t sequence = first; sequence <= last; ++sequence) {
        if (!eventually([&] { return ring.push(unknown_command(sequence)); },
                        Milliseconds{2'000})) {
            return false;
        }
    }
    return true;
}

/// True once slot 0's status shows `count` commands read, within `timeout`.
bool read_all(const Supervised& supervised, std::uint64_t count, Milliseconds timeout) {
    return eventually([&] { return supervised.state->ack(0).commands_read == count; }, timeout);
}

#ifdef __linux__

/// A thread's blocked and caught signals, from /proc.
struct ThreadSignals {
    std::uint64_t blocked = 0;
    std::uint64_t caught = 0;
};

/// The SigBlk and SigCgt masks in a /proc/<pid>/task/<tid>/status file, or
/// nothing if either is missing.
std::optional<ThreadSignals> thread_signals(const std::filesystem::path& status) {
    std::ifstream in{status};
    std::optional<std::uint64_t> blocked;
    std::optional<std::uint64_t> caught;
    for (std::string line; std::getline(in, line);) {
        constexpr std::string_view kBlocked = "SigBlk:";
        constexpr std::string_view kCaught = "SigCgt:";
        if (line.starts_with(kBlocked)) {
            blocked = std::stoull(line.substr(kBlocked.size()), nullptr, 16);
        } else if (line.starts_with(kCaught)) {
            caught = std::stoull(line.substr(kCaught.size()), nullptr, 16);
        }
    }
    if (!blocked || !caught) {
        return std::nullopt;
    }
    return ThreadSignals{.blocked = *blocked, .caught = *caught};
}

/// Whether `mask`, as /proc shows one, includes `signal`.
bool includes(std::uint64_t mask, int signal) {
    return ((mask >> static_cast<unsigned>(signal - 1)) & 1U) != 0;
}

#endif

} // namespace

TEST_CASE("Engine events: rejected commands, summarized", "[engine]") {
    const std::unique_ptr<LoggedEngine> engine = ks::logs::test::start_logged_engine();
    Supervised& supervised = *engine->supervised;
    Records records{*engine->log->segment};
    AttachedRing client = attach_ring(supervised);

    REQUIRE(client.ring.push(unknown_command(1)));
    CHECK(records.wait_for(LogLevel::Warning, "rejected 1 command from slot 0"));

    REQUIRE(write_unknown(client.ring, 2, 2'001));
    REQUIRE(read_all(supervised, 2'001, Milliseconds{10'000}));
    // A window's summary comes when it ends, a second after it opened.
    CHECK(eventually(
        [&] {
            records.read();
            return records.rejections().total >= 2'001;
        },
        Milliseconds{4'000}));
    const Rejections seen = records.rejections();
    CHECK(seen.total == 2'001);
    CHECK(seen.lines <= 6);
    CHECK(seen.all_warnings);
}

TEST_CASE("Engine events: a corrupt ring", "[engine]") {
    const std::unique_ptr<LoggedEngine> engine = ks::logs::test::start_logged_engine();
    Supervised& supervised = *engine->supervised;
    Records records{*engine->log->segment};
    const AttachedRing client = attach_ring(supervised);

    // The head 2,000 past the tail: more than the ring holds.
    ks::ipc::CommandRing& shared = client.ring.ring();
    const std::uint64_t tail = ks::ipc::load_shared(shared.tail, std::memory_order_acquire);
    ks::ipc::store_shared(shared.head, tail + 2'000, std::memory_order_release);

    CHECK(records.wait_for(LogLevel::Error, "the ring in slot 0 is corrupt"));
    CHECK(ks::processes::test::pings(*supervised.socket, 2));
}

TEST_CASE("Engine events: flushed when it stops", "[engine]") {
    const std::unique_ptr<LoggedEngine> engine = ks::logs::test::start_logged_engine();
    Supervised& supervised = *engine->supervised;
    Records records{*engine->log->segment};
    AttachedRing client = attach_ring(supervised);

    REQUIRE(client.ring.push(unknown_command(1)));
    REQUIRE(client.ring.push(unknown_command(2)));
    CHECK(records.wait_for(LogLevel::Warning, "from slot 0"));
    REQUIRE(write_unknown(client.ring, 3, 52));
    REQUIRE(read_all(supervised, 52, Milliseconds{2'000}));
    // Well within a second of the first line: its window is still open.
    supervised.engine.signal(SIGTERM);
    CHECK(supervised.engine.wait() == 0);

    records.read();
    const Rejections seen = records.rejections();
    CHECK(seen.total == 52);
    CHECK(seen.all_warnings);
    const std::optional<std::size_t> stopped = records.find("stopped");
    REQUIRE(stopped.has_value());
    CHECK(seen.last < stopped.value_or(0));
}

TEST_CASE("Engine process: signals blocked in every thread", "[engine]") {
#ifndef __linux__
    SKIP("Only Linux lets another process read a thread's signal mask.");
#else
    const std::unique_ptr<Supervised> engine = ks::client_rings::test::start_engine();
    // Playing: the device's thread is running.
    REQUIRE(ks::client_rings::test::keeps_beating(*engine->state));

    const std::filesystem::path tasks =
        std::filesystem::path{"/proc"} / std::to_string(engine->engine.pid()) / "task";
    std::size_t threads = 0;
    for (const std::filesystem::directory_entry& task :
         std::filesystem::directory_iterator{tasks}) {
        const std::optional<ThreadSignals> found = thread_signals(task.path() / "status");
        REQUIRE(found.has_value());
        const ThreadSignals signals = found.value_or(ThreadSignals{});
        CAPTURE(task.path().filename().string(), signals.blocked, signals.caught);
        for (const int signal : {SIGTERM, SIGINT, SIGHUP}) {
            CAPTURE(signal);
            CHECK(includes(signals.blocked, signal));
            CHECK_FALSE(includes(signals.caught, signal));
        }
        ++threads;
    }
    // At least the control thread and the device's.
    CHECK(threads >= 2);

    engine->engine.signal(SIGTERM);
    CHECK(engine->engine.wait() == 0);
#endif
}
