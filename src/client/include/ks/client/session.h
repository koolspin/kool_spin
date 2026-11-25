// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// A client's session with kool_spin (specs/ks-play/spec.md requirements 12
/// to 20): the connection to the supervisor, the engine-state segment mapped
/// read-only, and at most one command ring.
///
/// Everything here runs on the caller's thread, never on the audio thread.
/// Failures are returned, never thrown. A session is used from one thread at
/// a time.
///
/// When the connection is lost (kool_spin died, or dropped the session), the
/// session keeps its ring and its mapping of the engine's state
/// (specs/supervisor-crash/spec.md requirement 15): requests return Closed,
/// and send(), wait_applied(), and snapshot() work on. The engine plays on
/// without kool_spin, so the ring still controls it; but nothing says that
/// the engine itself is still there, except a heartbeat in snapshot() that
/// keeps growing.

#include <ks/client/command.h>
#include <ks/client/snapshot.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/state.h>

#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace ks::client {

using Milliseconds = std::chrono::milliseconds;

/// What went wrong.
enum class ErrorKind : std::uint8_t {
    /// An argument is invalid (such as a name of 65 bytes), and nothing was
    /// sent.
    InvalidArgument,
    /// There's no supervisor to talk to: the socket doesn't exist, nothing
    /// listens, or the listener isn't owned by the expected user.
    NotReachable,
    /// A wait ran out.
    Timeout,
    /// The connection is closed: every later request returns this too. Or
    /// the session is, with close(), and every later call does.
    Closed,
    /// The supervisor answered with an Error: see Error::code and text.
    Refused,
    /// A frame or message the connection or decoder refused. The session is
    /// over, and every later call returns the same error.
    Protocol,
    /// A segment the supervisor sent was refused: wrong kind, version, or
    /// size.
    BadSegment,
    /// The engine-state segment holds an invalid value: the text names the
    /// field or block.
    BadState,
    /// The call needs a ring, and the session has none.
    NoRing,
    /// The ring is full or corrupt.
    RingUnavailable,
    /// open_ring() was called twice.
    RingAlreadyOpen,
};

struct Error {
    ErrorKind kind = ErrorKind::Protocol;
    /// For Refused: the code of the supervisor's Error.
    ipc::ErrorCode code = ipc::ErrorCode::BadMessage;
    std::string text;

    friend bool operator==(const Error&, const Error&) = default;
};

template <typename T>
using Result = std::expected<T, Error>;

/// How wait_applied() ended.
enum class Outcome : std::uint8_t {
    /// The engine read the command, and has rejected nothing since the ring
    /// was opened.
    Applied,
    /// The engine rejected a command: see Applied::rejection.
    Rejected,
    /// The ring's slot is flagged corrupt.
    Corrupt,
    /// The wait ran out.
    TimedOut,
};

struct Applied {
    Outcome outcome = Outcome::TimedOut;
    /// For Rejected: the last rejection.
    Rejection rejection = Rejection::None;
};

/// What FileLoaded says.
struct LoadedFile {
    std::uint64_t track_id = 0;
    std::uint32_t sample_rate = 0;
    std::uint64_t estimated_frames = 0;
};

/// The longest name Session::connect() takes, in bytes (the same as Hello's).
inline constexpr std::size_t kMaxNameSize = ipc::kMaxClientNameSize;

class Session {
public:
    /// Connects to the supervisor in `runtime_dir`, with this process's user
    /// ID as the expected one, says Hello as `name`, waits for Welcome, and
    /// maps the engine-state segment read-only. Gives up after `timeout`.
    [[nodiscard]] static Result<Session> connect(std::string_view name,
                                                 const std::filesystem::path& runtime_dir,
                                                 Milliseconds timeout = Milliseconds{5'000});

    /// Test hook: connect() with another expected user ID, to test the
    /// refusal of a listener owned by someone else.
    [[nodiscard]] static Result<Session> connect_as(std::string_view name,
                                                    const std::filesystem::path& runtime_dir,
                                                    uid_t expected_uid,
                                                    Milliseconds timeout = Milliseconds{5'000});

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;
    /// Closes the connection, which detaches the ring.
    ~Session();

    /// Asks for a command ring and maps it read-write. Retries for up to
    /// `timeout` while the answer is Error Unavailable. Then waits, within
    /// the same `timeout`, until the engine's status block shows the slot
    /// attached (until then it still shows the slot's earlier occupant), and
    /// records the slot's rejected count for the first wait_applied(). A
    /// Timeout error if the slot never shows. Returns the ring's slot. A
    /// second call is an error.
    [[nodiscard]] Result<std::uint32_t> open_ring(Milliseconds timeout = Milliseconds{5'000});

    /// Writes `command` into the ring with the next sequence number (from 1,
    /// increasing by 1), and returns that number. Writes nothing and returns
    /// an error with no ring, or with the ring full or corrupt.
    [[nodiscard]] Result<std::uint64_t> send(const Command& command);

    /// Waits until the engine's status block shows this session's slot has
    /// read the command with `sequence` (`last_sequence` has reached it),
    /// polling at most every 1 ms. Reports what the engine did since the
    /// previous call, or since open_ring(): Rejected, with the last
    /// rejection, if the slot's rejected count has risen since then (the
    /// call keeps the new count); Corrupt, at once, if the slot is flagged
    /// corrupt; Applied otherwise; TimedOut if the command isn't read in
    /// time. Reads the slot through the seqlock and uses only its copy. An
    /// error only if there's no ring or the state can't be read.
    ///
    /// Once the connection is lost, the command also has to have left the
    /// ring: a session that kool_spin dropped has had its ring detached, and
    /// its slot may be another client's by then.
    [[nodiscard]] Result<Applied> wait_applied(std::uint64_t sequence,
                                               Milliseconds timeout = Milliseconds{2'000});

    /// A validated copy of the engine's state. Takes at most 10 ms in all:
    /// a seqlock that stays unstable that long is a BadState error naming
    /// the block.
    [[nodiscard]] Result<Snapshot> snapshot() const;

    /// Test hook: snapshot() of `state`, which a test fills itself, with
    /// `budget` as the limit on seqlock retries. Non-const because a
    /// seqlock reader needs a reference it can load atomically through; it
    /// never writes.
    [[nodiscard]] static Result<Snapshot> snapshot_from(ipc::EngineState& state,
                                                        Milliseconds budget = Milliseconds{10});

    /// Sends LoadFile and returns what FileLoaded says, or the Error. A
    /// `timeout` of 30 s covers the probe and the engine.
    [[nodiscard]] Result<LoadedFile> load_file(std::uint32_t deck, const std::string& path,
                                               Milliseconds timeout = Milliseconds{30'000});

    /// Sends UnloadDeck and waits for DeckUnloaded.
    [[nodiscard]] Result<void> unload_deck(std::uint32_t deck,
                                           Milliseconds timeout = Milliseconds{5'000});

    /// Sends Stop and waits for Done: kool_spin has stopped.
    [[nodiscard]] Result<void> stop(Milliseconds timeout = Milliseconds{10'000});

    /// Sends Ping and waits for Pong.
    [[nodiscard]] Result<void> ping(Milliseconds timeout = Milliseconds{5'000});

    /// The first queued DecodeEnded (at most 64 are queued), or the next
    /// one to arrive, or a Timeout error after `timeout`.
    [[nodiscard]] Result<ipc::msg::DecodeEnded>
    next_decode_ended(Milliseconds timeout = Milliseconds{5'000});

    /// False once a call has found the connection lost, or the session is
    /// over. Doesn't read the socket: it says what the last call found.
    [[nodiscard]] bool connected() const noexcept;

    /// Closes the connection, and lets go of the ring and the engine's state.
    /// Later calls return Closed.
    void close() noexcept;

private:
    struct Impl;
    explicit Session(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace ks::client
