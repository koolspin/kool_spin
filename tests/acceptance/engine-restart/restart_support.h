// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Helpers for the engine-restart acceptance tests (specs/engine-restart/
// spec.md): kool_spin on the paced backend with a recording segment, a client
// that can read the engine's decks and the recording, generated WAV files, and
// checks on the recorded output.

#include <ks/ipc/command.h>
#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/recording.h>
#include <ks/ipc/region.h>
#include <ks/ipc/runtime_dir.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>

#include "logs_support.h"
#include "processes_support.h"
#include "rings_support.h"
#include "segments_support.h"
#include "tracks_support.h"
#include <catch2/catch_test_macros.hpp>
#include <signal.h>
#include <sys/types.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace ks::engine_restart::test {

using processes::test::Clock;
using processes::test::eventually;
using processes::test::Milliseconds;
using processes::test::Process;
using processes::test::Received;

/// The engine's sample rate, and its period, in these tests.
inline constexpr std::uint32_t kRate = 44'100;
inline constexpr std::uint32_t kPeriod = 256;

/// True when the test is built with AddressSanitizer or ThreadSanitizer.
inline constexpr bool kSlowSanitizer =
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    true;
#elifdef __has_feature
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    true;
#else
    false;
#endif
#else
    false;
#endif

/// The restart time the spec allows (requirement 12 and the restart
/// criterion): a target of 1 s, 3 s under a sanitizer.
inline constexpr Milliseconds kRestartLimit =
    kSlowSanitizer ? Milliseconds{3'000} : Milliseconds{1'000};

/// How long a test waits for a restarted engine.
inline constexpr Milliseconds kRestartWait{5'000};

// ---- WAV files -----------------------------------------------------------------------

/// Writes `<dir>/<name>`: a mono, 16-bit, 44.1 kHz WAV of `seconds` seconds, whose
/// frame n is `value(n)`, from -1 to 1. Returns the path.
inline std::filesystem::path write_wav(const std::filesystem::path& dir, std::string_view name,
                                       std::uint32_t seconds,
                                       const std::function<double(std::uint64_t)>& value) {
    const std::uint64_t frames = std::uint64_t{seconds} * kRate;
    const auto put = [](std::ofstream& out, std::uint64_t number, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            out.put(static_cast<char>((number >> (8 * i)) & 0xFFU));
        }
    };
    const std::filesystem::path path = dir / name;
    std::ofstream out{path, std::ios::binary};
    out.write("RIFF", 4);
    put(out, 36 + (frames * 2), 4);
    out.write("WAVEfmt ", 8);
    put(out, 16, 4);
    put(out, 1, 2); // PCM
    put(out, 1, 2); // mono
    put(out, kRate, 4);
    put(out, std::uint64_t{kRate} * 2, 4);
    put(out, 2, 2);
    put(out, 16, 2);
    out.write("data", 4);
    put(out, frames * 2, 4);
    for (std::uint64_t n = 0; n < frames; ++n) {
        const double clipped = std::clamp(value(n), -1.0, 1.0);
        const auto sample = static_cast<std::int16_t>(std::lround(clipped * 32'767.0));
        put(out, static_cast<std::uint16_t>(sample), 2);
    }
    return path;
}

/// A 440 Hz sine at half scale: the track whose recording shows a gap.
inline std::filesystem::path write_sine(const std::filesystem::path& dir, std::string_view name,
                                        std::uint32_t seconds) {
    return write_wav(dir, name, seconds, [](std::uint64_t n) {
        return 0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(n) / kRate);
    });
}

/// A constant 0.25: the track whose recording shows a fade-in.
inline std::filesystem::path write_constant(const std::filesystem::path& dir, std::string_view name,
                                            std::uint32_t seconds) {
    return write_wav(dir, name, seconds, [](std::uint64_t) { return 0.25; });
}

// ---- kool_spin ------------------------------------------------------------------------

/// What a session runs with.
struct SessionOptions {
    /// The ks-worker executable.
    std::string worker{track_loading::test::kWorkerPath};
    /// The ks-engine executable (the probe, for the crash-loop tests).
    std::string engine{processes::test::kEnginePath};
    /// After the defaults: kool_spin's own options.
    std::vector<std::string> supervisor;
    /// After the paced backend's options: more for ks-engine.
    std::vector<std::string> engine_options;
    /// Start with --recording.
    bool recording = true;
    /// Wait for supervisor.sock. The crash-loop tests turn this off, because
    /// the probe may be gone before the test looks.
    bool wait_for_socket = true;
};

/// kool_spin in a runtime directory of its own, with ks-engine on the paced
/// backend, started through /bin/sh so that its standard error goes to a
/// file. Whatever happens, kool_spin and any engine left without it are
/// killed when this is destroyed.
class Session {
public:
    explicit Session(const SessionOptions& options = {}) {
        REQUIRE(!dir_.path().empty());
        std::vector<std::string> args{"--runtime-dir", dir_.path().string(), "--log-dir",
                                      logs().string(), "--engine",           options.engine,
                                      "--worker",      options.worker};
        if (options.recording) {
            args.emplace_back("--recording");
        }
        args.insert(args.end(), options.supervisor.begin(), options.supervisor.end());
        args.emplace_back("--");
        args.insert(args.end(), {"--backend", "paced", "--rate", std::to_string(kRate),
                                 "--channels", "2", "--period", std::to_string(kPeriod)});
        args.insert(args.end(), options.engine_options.begin(), options.engine_options.end());
        process_ = processes::test::start(
            "/bin/sh", logs::test::with_stderr(err(), processes::test::kKoolSpinPath, args));
        REQUIRE(process_.pid() > 0);
        if (options.wait_for_socket) {
            const std::filesystem::path socket = dir_.path() / "supervisor.sock";
            static_cast<void>(
                eventually([&] { return std::filesystem::exists(socket) || !process_.running(); }));
            CAPTURE(stderr_text());
            REQUIRE(std::filesystem::exists(socket));
            REQUIRE(process_.running());
        }
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

    /// The engine's process ID, from its lock; 0 if none is running.
    [[nodiscard]] pid_t engine() const {
        return processes::test::lock_holder(dir_.path(), ipc::kEngineLockName).value_or(0);
    }

    /// The session folder, once kool_spin has made it.
    [[nodiscard]] std::filesystem::path folder() const {
        const std::vector<std::filesystem::path> folders = logs::test::session_folders(logs());
        REQUIRE(folders.size() == 1);
        return folders.front();
    }
    [[nodiscard]] std::filesystem::path kool_spin_log() const { return folder() / "kool_spin.log"; }
    [[nodiscard]] std::filesystem::path engine_log() const { return folder() / "ks-engine.log"; }

    /// What kool_spin has written to standard error.
    [[nodiscard]] std::string stderr_text() const {
        std::ifstream in{err(), std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }

    /// True if, within `timeout`, an engine other than `old` holds the lock.
    [[nodiscard]] bool engine_replaced(pid_t old, Milliseconds timeout = kRestartWait) const {
        return eventually(
            [&] {
                const pid_t now = engine();
                return now > 0 && now != old;
            },
            timeout);
    }

private:
    ipc::test::TempDir dir_;
    processes::test::EngineReaper reaper_{dir_.path()};
    Process process_;
};

/// Kills `session`'s engine with SIGKILL, and returns its process ID and when.
inline std::pair<pid_t, Clock::time_point> kill_engine(const Session& session) {
    const pid_t pid = session.engine();
    REQUIRE(pid > 0);
    const Clock::time_point when = Clock::now();
    REQUIRE(::kill(pid, SIGKILL) == 0);
    return {pid, when};
}

// ---- The engine's state, deck by deck ---------------------------------------------------

/// An engine-state segment, mapped read-only: decks, mixer, status.
class State {
public:
    static std::expected<State, std::string> map(int fd) {
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
        return State{std::move(*region), *state};
    }

    /// Deck `deck`'s last block, if one has been published and could be read.
    [[nodiscard]] std::optional<ipc::DeckState> deck(std::size_t deck) const {
        if (deck >= ipc::kStateDecks) {
            return std::nullopt;
        }
        const std::optional<ipc::DeckState> block =
            ipc::SeqlockReader<ipc::DeckState>{std::span{state_->decks}[deck]}.try_read(1'000);
        if (!block || block->heartbeat == 0) {
            return std::nullopt;
        }
        return block;
    }

    [[nodiscard]] std::optional<ipc::MixerState> mixer() const {
        const std::optional<ipc::MixerState> block =
            ipc::SeqlockReader<ipc::MixerState>{state_->mixer}.try_read(1'000);
        if (!block || block->heartbeat == 0) {
            return std::nullopt;
        }
        return block;
    }

    [[nodiscard]] std::optional<ipc::EngineStatus> status() const {
        const std::optional<ipc::EngineStatus> block =
            ipc::SeqlockReader<ipc::EngineStatus>{state_->status}.try_read(1'000);
        if (!block || block->heartbeat == 0) {
            return std::nullopt;
        }
        return block;
    }

    [[nodiscard]] std::uint64_t heartbeat() const {
        const auto block = status();
        return block ? block->heartbeat : 0;
    }

    [[nodiscard]] ipc::ClientAck ack(std::size_t slot) const {
        const auto block = status();
        if (!block || slot >= ipc::kStateClients) {
            return ipc::ClientAck{};
        }
        return std::span{block->clients}[slot];
    }

private:
    State(ipc::Region region, ipc::EngineState& state) noexcept
        : region_{std::move(region)}, state_{&state} {}

    ipc::Region region_;
    ipc::EngineState* state_;
};

[[nodiscard]] inline double real_of(std::uint64_t code) noexcept {
    return std::bit_cast<double>(code);
}

[[nodiscard]] inline bool playing(const ipc::DeckState& deck) noexcept {
    return (deck.flags & ipc::kDeckPlaying) != 0;
}

[[nodiscard]] inline bool loaded(const ipc::DeckState& deck) noexcept {
    return (deck.flags & ipc::kDeckLoaded) != 0;
}

/// The value in `value`, which a test requires to be there. (clang-tidy
/// can't see through REQUIRE, so tests take the value this way.)
template <typename T>
T require(const std::optional<T>& value) {
    REQUIRE(value.has_value());
    return value.value_or(T{});
}

/// Host time, in nanoseconds of the steady clock, of `when`.
[[nodiscard]] inline std::uint64_t host_ns(Clock::time_point when) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(when.time_since_epoch()).count());
}

/// Where deck `deck`'s block says the deck is at host time `host_time`, if it
/// kept playing at its rate since the block.
[[nodiscard]] inline double position_at(const ipc::DeckState& block, std::uint64_t host_time,
                                        double rate = 1.0) noexcept {
    const double elapsed_s =
        (static_cast<double>(host_time) - static_cast<double>(block.host_time)) / 1e9;
    return real_of(block.position) +
           (elapsed_s * rate * static_cast<double>(block.track_sample_rate));
}

// ---- A client ---------------------------------------------------------------------------

/// A client that has said Hello: its connection and the engine's state.
struct Client {
    ipc::Connection connection;
    State state;
    /// The next request ID it uses; Hello took 1.
    std::uint32_t next_request = 2;
};

/// Connects to `dir`'s supervisor and says Hello as `name`.
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
    std::expected<State, std::string> state = State::map(welcome->fds.front().get());
    if (!state) {
        return std::unexpected{state.error()};
    }
    return Client{.connection = std::move(*connection), .state = std::move(*state)};
}

/// Sends `body` as `client`'s next request and returns the answer, skipping
/// notifications (a DecodeEnded or a TrackReleased has request ID 0).
template <typename Body>
std::optional<Received> ask(Client& client, Body body) {
    const std::uint32_t id = client.next_request++;
    if (!processes::test::send(client.connection,
                               ipc::Message{.request_id = id, .body = std::move(body)})) {
        return std::nullopt;
    }
    for (;;) {
        std::optional<Received> answer = processes::test::receive(client.connection);
        if (!answer || answer->message.request_id == id) {
            return answer;
        }
    }
}

/// Asks for a LoadFile of `path` onto `deck`: the FileLoaded, or an Error, or
/// nothing.
inline std::optional<Received> load(Client& client, std::uint32_t deck,
                                    const std::filesystem::path& path) {
    return ask(client, ipc::msg::LoadFile{.deck = deck, .path = path});
}

/// True if `answer` is a FileLoaded.
[[nodiscard]] inline bool is_loaded(const std::optional<Received>& answer) {
    return answer && std::holds_alternative<ipc::msg::FileLoaded>(answer->message.body);
}

/// The Error `answer` is, or nothing.
[[nodiscard]] inline std::optional<ipc::msg::Error>
error_of(const std::optional<Received>& answer) {
    if (!answer) {
        return std::nullopt;
    }
    if (const auto* const error = std::get_if<ipc::msg::Error>(&answer->message.body)) {
        return *error;
    }
    return std::nullopt;
}

/// Opens a ring for `client`, asking again while the supervisor says
/// Unavailable.
inline client_rings::test::OpenedRing open_ring(Client& client) {
    std::expected<client_rings::test::OpenedRing, client_rings::test::OpenFailure> opened =
        client_rings::test::open_ring_retrying(client.connection, client.next_request);
    REQUIRE(opened.has_value());
    return std::move(*opened);
}

/// A recording segment, mapped read-only.
class Recording {
public:
    /// Asks for the recording with an OpenRecording.
    static std::expected<Recording, std::string> open(Client& client) {
        const std::optional<Received> answer = ask(client, ipc::msg::OpenRecording{});
        if (!answer) {
            return std::unexpected{std::string{"no answer"}};
        }
        if (const auto error = error_of(answer)) {
            return std::unexpected{"refused: " + error->text};
        }
        if (!std::holds_alternative<ipc::msg::RecordingOpened>(answer->message.body) ||
            answer->fds.size() != 1) {
            return std::unexpected{std::string{"not a RecordingOpened with one descriptor"}};
        }
        std::expected<ipc::Region, ipc::MapFailure> region =
            ipc::Region::map(answer->fds.front().get(), ipc::SegmentKind::Recording,
                             ipc::kRecordingLayoutVersion, ipc::Access::ReadOnly);
        if (!region) {
            return std::unexpected{std::string{"not a recording segment"}};
        }
        auto* const segment = region->at<ipc::RecordingSegment>(ipc::kSegmentHeaderSize);
        if (segment == nullptr) {
            return std::unexpected{std::string{"too small for a recording"}};
        }
        return Recording{std::move(*region), *segment};
    }

    [[nodiscard]] const ipc::RecordingReader& reader() const noexcept { return reader_; }

    /// Frames [first, first + count): the left channel only. Nothing if
    /// they're not there.
    [[nodiscard]] std::optional<std::vector<float>> left(std::uint64_t first,
                                                         std::uint64_t count) const {
        const auto frames = reader_.read(first, count);
        if (!frames) {
            return std::nullopt;
        }
        std::vector<float> out;
        out.reserve(static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < frames->size(); i += ipc::kRecordingChannels) {
            out.push_back((*frames)[i]);
        }
        return out;
    }

private:
    Recording(ipc::Region region, ipc::RecordingSegment& segment) noexcept
        : region_{std::move(region)}, reader_{segment} {}

    ipc::Region region_;
    ipc::RecordingReader reader_;
};

// ---- Playing ----------------------------------------------------------------------------

/// A session with a client, its ring (slot 0), the recording, and a file on
/// deck 0, not yet playing.
struct Playing {
    Session session;
    Client client;
    Recording recording;
    client_rings::test::OpenedRing ring;
    std::uint64_t sequence = 1;

    /// Starts kool_spin, joins, opens the recording and a ring, and loads
    /// `file` onto deck 0.
    explicit Playing(const std::filesystem::path& file, const SessionOptions& options = {})
        : session{options}, client{joined(session)}, recording{opened(client)},
          ring{open_ring(client)} {
        REQUIRE(ring.slot == 0);
        REQUIRE(is_loaded(load(client, 0, file)));
    }

    /// Sends `command` through the ring, numbering it.
    void send(ipc::CommandType type, std::uint32_t target, double value) {
        ipc::CommandSlot command = client_rings::test::command(type, value, sequence++);
        command.target = target;
        REQUIRE(ring.ring.push(command));
    }

    void play(std::uint32_t deck, bool on = true) {
        REQUIRE(ring.ring.push(track_loading::test::set_playing(deck, on, sequence++)));
    }

private:
    static Client joined(const Session& session) {
        std::expected<Client, std::string> client = join(session.dir(), "chaos");
        REQUIRE(client.has_value());
        return std::move(*client);
    }

    static Recording opened(Client& client) {
        std::expected<Recording, std::string> recording = Recording::open(client);
        REQUIRE(recording.has_value());
        return std::move(*recording);
    }
};

/// A `Playing` for `file`.
inline std::unique_ptr<Playing> start_playing(const std::filesystem::path& file,
                                              const SessionOptions& options = {}) {
    return std::make_unique<Playing>(file, options);
}

// ---- What the recording shows -------------------------------------------------------------

/// The positions, in frames and to a fraction of one, at which `samples` goes
/// from negative to non-negative.
[[nodiscard]] inline std::vector<double> rising_crossings(std::span<const float> samples) {
    std::vector<double> crossings;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const auto before = static_cast<double>(samples[i - 1]);
        const auto after = static_cast<double>(samples[i]);
        if (before < 0.0 && after >= 0.0) {
            crossings.push_back(static_cast<double>(i - 1) + ((-before) / (after - before)));
        }
    }
    return crossings;
}

/// What "no gap" means for a 440 Hz sine (spec, chaos criterion): the rising
/// zero crossings are `kRate / 440` frames apart, within one frame, all the
/// way through `samples`, which must hold at least `minimum_crossings`.
struct GapCheck {
    bool ok = false;
    std::size_t crossings = 0;
    /// The widest and narrowest spacing seen, in frames.
    double widest = 0.0;
    double narrowest = 0.0;
};

[[nodiscard]] inline GapCheck check_no_gap(std::span<const float> samples,
                                           std::size_t minimum_crossings) {
    const std::vector<double> crossings = rising_crossings(samples);
    GapCheck check{.ok = crossings.size() >= minimum_crossings, .crossings = crossings.size()};
    constexpr double kSpacing = static_cast<double>(kRate) / 440.0;
    check.narrowest = kSpacing;
    check.widest = kSpacing;
    for (std::size_t i = 1; i < crossings.size(); ++i) {
        const double gap = crossings[i] - crossings[i - 1];
        check.widest = std::max(check.widest, gap);
        check.narrowest = std::min(check.narrowest, gap);
        if (std::abs(gap - kSpacing) > 1.0) {
            check.ok = false;
        }
    }
    return check;
}

/// The recording from frame `first` to the newest frame, left channel, for
/// at most `limit` frames. Empty if it can't be read.
[[nodiscard]] inline std::vector<float>
recorded_from(const Recording& recording, std::uint64_t first, std::uint64_t limit = 1'000'000) {
    const std::uint64_t written = recording.reader().frames_written();
    if (written <= first) {
        return {};
    }
    const std::uint64_t count = std::min(written - first, limit);
    return recording.left(first, count).value_or(std::vector<float>{});
}

} // namespace ks::engine_restart::test
