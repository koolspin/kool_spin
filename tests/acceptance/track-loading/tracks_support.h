// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by specs/track-loading/spec.md's acceptance tests: the decks as the
// state segment shows them, commands, track segments the test writes as a
// worker would, ks-engine with the test as its supervisor, kool_spin with its
// logs and a worker of the test's choice, fake tracks for ks_fake_worker,
// LoadFile, and, on Linux, what /proc says about a process's mappings. Built
// on the logs, client-rings, and processes tests' helpers.

#include <ks/ipc/command.h>
#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/frame.h>
#include <ks/ipc/layout.h>
#include <ks/ipc/log.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/region.h>
#include <ks/ipc/segment.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>
#include <ks/ipc/track_segment.h>

#include "logs_support.h"
#include "processes_support.h"
#include "rings_support.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/sysmacros.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace ks::track_loading::test {

using processes::test::Clock;
using processes::test::eventually;
using processes::test::Milliseconds;
using processes::test::Process;
using processes::test::Received;

/// ks-worker, and the fake that stands in for it.
inline constexpr std::string_view kWorkerPath = KS_WORKER_PATH;
inline constexpr std::string_view kFakeWorkerPath = KS_FAKE_WORKER_PATH;

/// "A deck shows" (Acceptance criteria): within 5 s.
inline constexpr Milliseconds kShowWait{5'000};

/// How far a deck waiting at the decode limit stays short of it, in frames
/// (engine::kInterpolatorReach).
inline constexpr double kReach = 128.0;

/// `text` as a whole number in `base`, or nothing.
template <typename T>
std::optional<T> parse_number(std::string_view text, int base = 10) {
    T value{};
    const char* const first = std::to_address(text.begin());
    const char* const last = std::to_address(text.end());
    const auto [end, error] = std::from_chars(first, last, value, base);
    if (text.empty() || error != std::errc{} || end != last) {
        return std::nullopt;
    }
    return value;
}

// ---- The decks -------------------------------------------------------------------

/// One deck as its state block shows it.
struct DeckView {
    std::uint64_t track_id = 0;
    std::uint64_t sample_rate = 0;
    std::uint64_t frames_ready = 0;
    bool loaded = false;
    bool playing = false;
    bool waiting = false;
    bool complete = false;
    /// In track frames.
    double position = 0.0;
};

/// An engine-state segment, mapped read-only, read for its decks and status.
class Decks {
public:
    /// Maps `fd` as an engine-state segment, or says why it couldn't.
    static std::expected<Decks, std::string> map(int fd) {
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
        return Decks{std::move(*region), *state};
    }

    /// Deck `deck` (from 0) as its last block shows it, or nothing if none
    /// has been published, or it couldn't be read.
    [[nodiscard]] std::optional<DeckView> deck(std::size_t deck) const {
        if (deck >= ipc::kStateDecks) {
            return std::nullopt;
        }
        const std::optional<ipc::DeckState> state =
            ipc::SeqlockReader<ipc::DeckState>{std::span{state_->decks}[deck]}.try_read(1'000);
        if (!state || state->heartbeat == 0) {
            return std::nullopt;
        }
        return DeckView{.track_id = state->track_id,
                        .sample_rate = state->track_sample_rate,
                        .frames_ready = state->frames_ready,
                        .loaded = (state->flags & ipc::kDeckLoaded) != 0,
                        .playing = (state->flags & ipc::kDeckPlaying) != 0,
                        .waiting = (state->flags & ipc::kDeckWaiting) != 0,
                        .complete = (state->flags & ipc::kDeckComplete) != 0,
                        .position = std::bit_cast<double>(state->position)};
    }

    /// The status block's heartbeat; 0 if there's none yet.
    [[nodiscard]] std::uint64_t heartbeat() const {
        const std::optional<ipc::EngineStatus> status =
            ipc::SeqlockReader<ipc::EngineStatus>{state_->status}.try_read(1'000);
        return status ? status->heartbeat : 0;
    }

private:
    Decks(ipc::Region region, ipc::EngineState& state) noexcept
        : region_{std::move(region)}, state_{&state} {}

    ipc::Region region_;
    ipc::EngineState* state_;
};

/// True if, within `timeout`, deck `deck` shows something `done` accepts.
/// `last` gets the last view read, for CAPTURE.
template <typename Done>
bool deck_shows(const Decks& decks, std::size_t deck, Done done, DeckView& last,
                Milliseconds timeout = kShowWait) {
    return eventually(
        [&] {
            const std::optional<DeckView> view = decks.deck(deck);
            if (!view) {
                return false;
            }
            last = *view;
            return done(*view);
        },
        timeout);
}

/// True if a first heartbeat appears within 5 s, and then the heartbeat
/// increases between each check, 100 ms apart, for `duration`.
inline bool keeps_beating(const Decks& decks, Milliseconds duration = Milliseconds{500}) {
    std::uint64_t last = 0;
    if (!eventually([&] {
            last = decks.heartbeat();
            return last != 0;
        })) {
        return false;
    }
    for (Milliseconds waited{0}; waited < duration; waited += Milliseconds{100}) {
        std::this_thread::sleep_for(Milliseconds{100});
        const std::uint64_t now = decks.heartbeat();
        if (now <= last) {
            return false;
        }
        last = now;
    }
    return true;
}

/// True if deck `deck`'s position increases over 300 ms, within 5 s.
inline bool position_advances(const Decks& decks, std::size_t deck) {
    return eventually([&] {
        const std::optional<DeckView> before = decks.deck(deck);
        std::this_thread::sleep_for(Milliseconds{300});
        const std::optional<DeckView> after = decks.deck(deck);
        return before && after && after->position > before->position;
    });
}

// ---- Commands --------------------------------------------------------------------

/// SetPlaying on `deck`: 1 plays, 0 pauses.
inline ipc::CommandSlot set_playing(std::uint32_t deck, bool playing, std::uint64_t sequence) {
    return ipc::CommandSlot{.sequence = sequence,
                            .type = static_cast<std::uint32_t>(ipc::CommandType::SetPlaying),
                            .target = deck,
                            .payload = {playing ? 1U : 0U, 0, 0, 0, 0, 0}};
}

// ---- Track segments the test writes ------------------------------------------------

/// The value the test writes in every published sample.
inline constexpr float kSampleValue = 0.25F;

/// A track segment that the test created and writes, as a worker would.
class TestTrack {
public:
    /// Creates a segment with `factory` and maps it read-write.
    static std::unique_ptr<TestTrack> create(ipc::SegmentFactory& factory,
                                             std::uint32_t version = ipc::kTrackLayoutVersion,
                                             std::size_t size = ipc::kTrackSegmentSize) {
        std::expected<ipc::Fd, std::error_code> fd =
            factory.create(ipc::SegmentKind::Track, version, size);
        REQUIRE(fd.has_value());
        std::expected<ipc::Region, ipc::MapFailure> region =
            ipc::Region::map(fd->get(), ipc::SegmentKind::Track, version, ipc::Access::ReadWrite);
        REQUIRE(region.has_value());
        return std::make_unique<TestTrack>(std::move(*fd), std::move(*region));
    }

    /// Takes over a segment mapped read-write. Use create().
    TestTrack(ipc::Fd fd, ipc::Region region)
        : fd_{std::move(fd)}, region_{std::move(region)},
          writer_{std::make_unique<ipc::TrackWriter>(region_.bytes())} {}

    TestTrack(const TestTrack&) = delete;
    TestTrack& operator=(const TestTrack&) = delete;
    TestTrack(TestTrack&&) = delete;
    TestTrack& operator=(TestTrack&&) = delete;
    ~TestTrack() = default;

    [[nodiscard]] const ipc::Fd& fd() const noexcept { return fd_; }

    /// The header, for tests that write its fields themselves.
    [[nodiscard]] ipc::TrackHeader& header() const {
        auto* const header = region_.at<ipc::TrackHeader>(ipc::kSegmentHeaderSize);
        REQUIRE(header != nullptr);
        return *header;
    }

    /// Stores the rate.
    void start(std::uint32_t rate) noexcept { writer_->start(rate); }

    /// Writes kSampleValue into frames [published, frames), then publishes
    /// `frames`.
    void publish(std::uint64_t frames) {
        fill(frames);
        writer_->publish(frames);
    }

    /// Writes the frames up to `frames`, as publish() does, then finishes the
    /// track with `status`.
    void finish(ipc::TrackStatus status, std::uint64_t frames) {
        fill(frames);
        writer_->finish(status, frames);
    }

private:
    void fill(std::uint64_t frames) {
        REQUIRE(frames <= writer_->capacity());
        if (frames > written_) {
            std::ranges::fill(
                writer_->samples().subspan(static_cast<std::size_t>(written_) * 2,
                                           static_cast<std::size_t>(frames - written_) * 2),
                kSampleValue);
            written_ = frames;
        }
    }

    ipc::Fd fd_;
    ipc::Region region_;
    std::unique_ptr<ipc::TrackWriter> writer_;
    std::uint64_t written_ = 0;
};

// ---- ks-engine, with the test as its supervisor --------------------------------------

/// The lines an engine wrote to its log segment, read as they come.
class EngineLog {
public:
    explicit EngineLog(ipc::LogSegment& segment) : reader_{segment} {}

    /// True if, within `timeout`, the engine has logged a line at `level`
    /// whose text contains `phrase`.
    bool has(ipc::LogLevel level, std::string_view phrase,
             Milliseconds timeout = Milliseconds{2'000}) {
        return eventually(
            [&] {
                read();
                return std::ranges::any_of(lines_, [&](const auto& line) {
                    return line.first == level && line.second.find(phrase) != std::string::npos;
                });
            },
            timeout);
    }

    /// How many lines at `level` contain `phrase`, of those read so far.
    std::size_t count(ipc::LogLevel level, std::string_view phrase) {
        read();
        return static_cast<std::size_t>(std::ranges::count_if(lines_, [&](const auto& line) {
            return line.first == level && line.second.find(phrase) != std::string::npos;
        }));
    }

    /// Every line read so far, for CAPTURE.
    [[nodiscard]] std::string all() {
        read();
        std::string text;
        for (const auto& line : lines_) {
            text += line.second;
            text += '\n';
        }
        return text;
    }

private:
    void read() {
        while (const std::optional<ipc::LogRecord> record = reader_.next()) {
            lines_.emplace_back(static_cast<ipc::LogLevel>(record->level),
                                logs::test::text_of(*record));
        }
    }

    ipc::LogReader reader_;
    std::vector<std::pair<ipc::LogLevel, std::string>> lines_;
};

/// ks-engine with the test as its supervisor and a log segment, its decks,
/// and its log.
struct TestEngine {
    std::unique_ptr<logs::test::LoggedEngine> logged;
    std::unique_ptr<Decks> decks;
    std::unique_ptr<EngineLog> log;
    std::uint32_t next_request = 1;

    [[nodiscard]] client_rings::test::Supervised& supervised() const { return *logged->supervised; }
    [[nodiscard]] ipc::Connection& socket() const { return *logged->supervised->socket; }
    [[nodiscard]] ipc::SegmentFactory& factory() const { return *logged->supervised->factory; }
    [[nodiscard]] Process& process() const { return logged->supervised->engine; }
};

/// Starts ks-engine with a log segment, as kool_spin would, on the null
/// backend, and waits for its Ready.
inline std::unique_ptr<TestEngine> start_engine() {
    auto engine = std::make_unique<TestEngine>();
    engine->logged = logs::test::start_logged_engine();
    std::expected<Decks, std::string> decks = Decks::map(engine->supervised().state_fd.get());
    REQUIRE(decks.has_value());
    engine->decks = std::make_unique<Decks>(std::move(*decks));
    engine->log = std::make_unique<EngineLog>(*engine->logged->log->segment);
    return engine;
}

/// The messages that arrive within `timeout` of each other, after the first
/// within 5 s, until `enough` returns true for what has arrived.
template <typename Enough>
std::vector<Received> receive_until(ipc::Connection& connection, Enough enough) {
    std::vector<Received> received;
    static_cast<void>(eventually([&] {
        std::expected<std::optional<ipc::ReceivedFrame>, ipc::ConnectionFailure> frame =
            connection.receive();
        if (frame && *frame) {
            std::expected<ipc::Message, ipc::MessageError> message = ipc::decode((*frame)->frame);
            if (message) {
                received.push_back(
                    Received{.message = std::move(*message), .fds = std::move((*frame)->fds)});
            }
        }
        return enough(received);
    }));
    return received;
}

/// Sends the engine a LoadTrack for `segment`, and returns the answer to it:
/// the first message with its request ID. Notifications that arrive first
/// are skipped.
inline std::optional<Received> load_track(TestEngine& engine, const ipc::Fd& segment,
                                          std::uint64_t track_id, std::uint32_t deck,
                                          std::uint32_t rate) {
    const std::uint32_t id = engine.next_request++;
    const std::array<int, 1> fds{segment.get()};
    if (!processes::test::send(engine.socket(),
                               ipc::Message{.request_id = id,
                                            .body = ipc::msg::LoadTrack{.track_id = track_id,
                                                                        .deck = deck,
                                                                        .sample_rate = rate}},
                               fds)) {
        return std::nullopt;
    }
    const std::vector<Received> received =
        receive_until(engine.socket(), [id](const std::vector<Received>& so_far) {
            return std::ranges::any_of(
                so_far, [id](const Received& r) { return r.message.request_id == id; });
        });
    for (const Received& message : received) {
        if (message.message.request_id == id) {
            return Received{.message = message.message, .fds = {}};
        }
    }
    return std::nullopt;
}

/// True if `answer` is a TrackLoaded.
inline bool track_loaded(const std::optional<Received>& answer) {
    return answer && std::holds_alternative<ipc::msg::TrackLoaded>(answer->message.body);
}

/// True if `answer` is an Error with `code`.
inline bool is_error(const std::optional<Received>& answer, ipc::ErrorCode code) {
    return processes::test::error_code(answer) == code;
}

/// Attaches a command ring to the engine, as kool_spin would for a client,
/// and returns it, mapped.
inline client_rings::test::ClientRing attach_ring(TestEngine& engine) {
    const ipc::Fd segment = client_rings::test::create_segment(engine.supervised());
    const std::optional<Received> attached =
        client_rings::test::attach(engine.supervised(), segment, engine.next_request++);
    REQUIRE(client_rings::test::slot_of(attached).has_value());
    std::expected<client_rings::test::ClientRing, std::string> ring =
        client_rings::test::ClientRing::map(segment.get());
    REQUIRE(ring.has_value());
    return std::move(*ring);
}

// ---- kool_spin, with logs and a worker of the test's choice ---------------------------

/// kool_spin in a runtime directory of its own, with ks-engine on the null
/// backend and `worker` as its worker, started through /bin/sh so that its
/// standard error goes to a file. Its logs go in `<dir>/logs`. Whatever
/// happens, kool_spin and any engine left without it are killed when this
/// is destroyed.
class Session {
public:
    explicit Session(std::string_view worker = kWorkerPath, std::vector<std::string> options = {}) {
        REQUIRE(!dir_.path().empty());
        std::vector<std::string> args{"--runtime-dir", dir_.path().string(),
                                      "--log-dir",     logs().string(),
                                      "--engine",      std::string{processes::test::kEnginePath},
                                      "--worker",      std::string{worker}};
        args.insert(args.end(), options.begin(), options.end());
        args.emplace_back("--");
        const std::vector<std::string> engine = processes::test::null_backend();
        args.insert(args.end(), engine.begin(), engine.end());
        process_ = processes::test::start(
            "/bin/sh", logs::test::with_stderr(err(), processes::test::kKoolSpinPath, args));
        REQUIRE(process_.pid() > 0);
        const std::filesystem::path socket = dir_.path() / "supervisor.sock";
        static_cast<void>(
            eventually([&] { return std::filesystem::exists(socket) || !process_.running(); }));
        CAPTURE(stderr_text());
        REQUIRE(std::filesystem::exists(socket));
        REQUIRE(process_.running());
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session() = default;

    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_.path(); }
    [[nodiscard]] std::filesystem::path logs() const { return dir_.path() / "logs"; }
    [[nodiscard]] std::filesystem::path err() const { return dir_.path() / "stderr"; }
    [[nodiscard]] Process& process() noexcept { return process_; }

    /// The session folder.
    [[nodiscard]] std::filesystem::path folder() const {
        const std::vector<std::filesystem::path> folders = logs::test::session_folders(logs());
        REQUIRE(folders.size() == 1);
        return folders.front();
    }
    [[nodiscard]] std::filesystem::path kool_spin_log() const { return folder() / "kool_spin.log"; }
    [[nodiscard]] std::filesystem::path engine_log() const { return folder() / "ks-engine.log"; }
    [[nodiscard]] std::filesystem::path worker_log() const { return folder() / "ks-worker.log"; }

    /// What kool_spin has written to standard error.
    [[nodiscard]] std::string stderr_text() const {
        std::ifstream in{err(), std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }

private:
    ipc::test::TempDir dir_;
    processes::test::EngineReaper reaper_{dir_.path()};
    Process process_;
};

/// A client that has said Hello: its connection, and the decks.
struct TrackClient {
    ipc::Connection connection;
    std::unique_ptr<Decks> decks;
    /// The next request ID it uses; Hello took 1.
    std::uint32_t next_request = 2;
};

/// Connects to `session`'s supervisor and says Hello as `name`.
inline TrackClient join(const Session& session, std::string name) {
    std::expected<ipc::Connection, std::error_code> connection =
        processes::test::connect(session.dir());
    REQUIRE(connection.has_value());
    const ipc::Message hello{.request_id = 1, .body = ipc::msg::Hello{std::move(name)}};
    REQUIRE(processes::test::send(*connection, hello));
    const std::optional<Received> welcome = processes::test::receive(*connection);
    REQUIRE(processes::test::is<ipc::msg::Welcome>(welcome, 1));
    const int state = welcome && welcome->fds.size() == 1 ? welcome->fds.front().get() : -1;
    REQUIRE(state >= 0);
    std::expected<Decks, std::string> decks = Decks::map(state);
    REQUIRE(decks.has_value());
    return TrackClient{.connection = std::move(*connection),
                       .decks = std::make_unique<Decks>(std::move(*decks)),
                       .next_request = 2};
}

/// Sends a LoadFile of `path` for `deck`, and returns its request ID.
inline std::uint32_t send_load(TrackClient& client, std::uint32_t deck,
                               const std::filesystem::path& path) {
    const std::uint32_t id = client.next_request++;
    REQUIRE(processes::test::send(
        client.connection,
        ipc::Message{.request_id = id, .body = ipc::msg::LoadFile{.deck = deck, .path = path}}));
    return id;
}

/// Sends a LoadFile of `path` for `deck`, and waits up to 5 s for the answer.
inline std::optional<Received> load(TrackClient& client, std::uint32_t deck,
                                    const std::filesystem::path& path) {
    const std::uint32_t id = send_load(client, deck, path);
    std::optional<Received> answer = processes::test::receive(client.connection);
    // A DecodeEnded (specs/ks-play/spec.md requirement 8) is a notification
    // about an earlier load, not the answer.
    while (answer && std::holds_alternative<ipc::msg::DecodeEnded>(answer->message.body)) {
        answer = processes::test::receive(client.connection);
    }
    if (answer && answer->message.request_id != id) {
        return std::nullopt;
    }
    return answer;
}

/// The FileLoaded `answer` is, or nothing.
inline std::optional<ipc::msg::FileLoaded> file_loaded(const std::optional<Received>& answer) {
    if (!answer) {
        return std::nullopt;
    }
    const auto* const loaded = std::get_if<ipc::msg::FileLoaded>(&answer->message.body);
    if (loaded == nullptr) {
        return std::nullopt;
    }
    return *loaded;
}

/// The Error `answer` is, or nothing.
inline std::optional<ipc::msg::Error> error_of(const std::optional<Received>& answer) {
    if (!answer) {
        return std::nullopt;
    }
    const auto* const error = std::get_if<ipc::msg::Error>(&answer->message.body);
    if (error == nullptr) {
        return std::nullopt;
    }
    return *error;
}

/// Opens a ring for `client`.
inline client_rings::test::ClientRing open_ring(TrackClient& client) {
    std::expected<client_rings::test::OpenedRing, client_rings::test::OpenFailure> opened =
        client_rings::test::open_ring_retrying(client.connection, client.next_request);
    REQUIRE(opened.has_value());
    return std::move(opened->ring);
}

/// The worker process IDs that `kool_spin_log` reports, from its
/// "started ks-worker (process <pid>) for track <id>" lines, by track ID.
inline std::vector<std::pair<std::uint64_t, pid_t>>
worker_pids(const std::filesystem::path& kool_spin_log) {
    std::vector<std::pair<std::uint64_t, pid_t>> pids;
    constexpr std::string_view kStarted = "started ks-worker (process ";
    constexpr std::string_view kFor = ") for track ";
    for (const std::string& line : logs::test::read_lines(kool_spin_log)) {
        const std::size_t at = line.find(kStarted);
        if (at == std::string::npos) {
            continue;
        }
        const std::size_t pid_at = at + kStarted.size();
        const std::size_t for_at = line.find(kFor, pid_at);
        if (for_at == std::string::npos) {
            continue;
        }
        const std::string_view text{line};
        const std::optional<pid_t> pid = parse_number<pid_t>(text.substr(pid_at, for_at - pid_at));
        const std::optional<std::uint64_t> track =
            parse_number<std::uint64_t>(text.substr(for_at + kFor.size()));
        if (pid && track && *pid > 0) {
            pids.emplace_back(*track, *pid);
        }
    }
    return pids;
}

// ---- Fake tracks for ks_fake_worker --------------------------------------------------

/// What a fake track asks ks_fake_worker to do (fake_worker.cpp).
struct FakeTrack {
    std::uint32_t rate = 44'100;
    std::uint64_t estimate = 0;
    std::uint64_t probe_delay_ms = 0;
    /// Never answer Probe.
    bool probe_hang = false;
    /// If not empty, refuse the Probe with this text. Initialized so that
    /// tests can leave it out of designated initializers
    /// (-Wmissing-designated-field-initializers).
    std::string refusal{}; // NOLINT(readability-redundant-member-init): see above
    /// How long it waits after Decode before it stores the rate.
    std::uint64_t decode_delay_ms = 0;
    std::uint64_t frames = 0;
    std::uint64_t chunk_frames = 4'096;
    std::uint64_t chunk_ms = 0;
    /// "complete", "failed", "crash", or "hang".
    std::string end = "complete";
    bool ignore_sigterm = false;
};

/// Writes `fake` to `<dir>/<name>`, and returns the path.
inline std::filesystem::path write_fake(const std::filesystem::path& dir, std::string_view name,
                                        const FakeTrack& fake) {
    std::ostringstream text;
    text << "ks-fake-track\n"
         << "rate " << fake.rate << '\n'
         << "estimate " << fake.estimate << '\n'
         << "probe-delay " << fake.probe_delay_ms << '\n';
    if (fake.probe_hang) {
        text << "probe hang\n";
    } else if (!fake.refusal.empty()) {
        text << "probe refuse " << fake.refusal << '\n';
    }
    if (fake.decode_delay_ms != 0) {
        text << "decode-delay " << fake.decode_delay_ms << '\n';
    }
    text << "frames " << fake.frames << '\n'
         << "chunk " << fake.chunk_frames << ' ' << fake.chunk_ms << '\n'
         << "end " << fake.end << '\n';
    if (fake.ignore_sigterm) {
        text << "ignore-sigterm\n";
    }
    const std::filesystem::path path = dir / name;
    logs::test::write_file(path, text.str());
    return path;
}

// ---- /proc, on Linux --------------------------------------------------------------

#ifdef __linux__

/// Which file a mapping maps, as /proc/<pid>/maps shows it.
struct FileKey {
    unsigned major = 0;
    unsigned minor = 0;
    std::uint64_t inode = 0;
};

/// The file `fd` refers to.
inline FileKey key_of(int fd) {
    struct stat status{};
    REQUIRE(::fstat(fd, &status) == 0);
    return FileKey{.major = ::major(status.st_dev),
                   .minor = ::minor(status.st_dev),
                   .inode = static_cast<std::uint64_t>(status.st_ino)};
}

/// True if `line`, a line of /proc/<pid>/maps or a mapping's first line in
/// /proc/<pid>/smaps, maps the file `key`.
inline bool maps_key(const std::string& line, const FileKey& key) {
    std::istringstream words{line};
    std::string range;
    std::string perms;
    std::string offset;
    std::string device;
    std::uint64_t inode = 0;
    if (!(words >> range >> perms >> offset >> device >> inode)) {
        return false;
    }
    const std::size_t colon = device.find(':');
    if (colon == std::string::npos || range.find('-') == std::string::npos) {
        return false;
    }
    const std::string_view text{device};
    const std::optional<unsigned> major = parse_number<unsigned>(text.substr(0, colon), 16);
    const std::optional<unsigned> minor = parse_number<unsigned>(text.substr(colon + 1), 16);
    return major && minor && *major == key.major && *minor == key.minor && inode == key.inode &&
           inode != 0;
}

/// True if process `pid` maps the file `key`.
inline bool maps_file(pid_t pid, const FileKey& key) {
    std::ifstream maps{"/proc/" + std::to_string(pid) + "/maps"};
    REQUIRE(maps.good());
    for (std::string line; std::getline(maps, line);) {
        if (maps_key(line, key)) {
            return true;
        }
    }
    return false;
}

/// The resident memory of process `pid`'s mappings of the file `key`, in kB,
/// from /proc/<pid>/smaps; nothing if it doesn't map it.
inline std::optional<std::uint64_t> rss_kb(pid_t pid, const FileKey& key) {
    std::ifstream smaps{"/proc/" + std::to_string(pid) + "/smaps"};
    REQUIRE(smaps.good());
    std::optional<std::uint64_t> total;
    bool inside = false;
    for (std::string line; std::getline(smaps, line);) {
        // A mapping's first line starts with its address range; the fields
        // after it start with a name and a colon.
        const std::size_t space = line.find(' ');
        const bool header = space != std::string::npos && line.find('-') < space;
        if (header) {
            inside = maps_key(line, key);
            continue;
        }
        if (inside && line.starts_with("Rss:")) {
            std::istringstream words{line.substr(4)};
            std::uint64_t kb = 0;
            if (words >> kb) {
                total = total.value_or(0) + kb;
            }
        }
    }
    return total;
}

#endif

} // namespace ks::track_loading::test
