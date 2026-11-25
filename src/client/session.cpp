// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/client/session.h>

#include <ks/ipc/command.h>
#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/frame.h>
#include <ks/ipc/listener.h>
#include <ks/ipc/region.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/spsc_ring.h>

#include "decode_queue.h"
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <optional>
#include <span>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace ks::client {

namespace {

using Clock = std::chrono::steady_clock;
using Producer = ipc::RingProducer<ipc::CommandSlot, ipc::kCommandRingCapacity>;

/// The most a snapshot() takes in all (requirement 17), and the most one
/// read of the status block takes.
constexpr Milliseconds kSnapshotBudget{10};
/// How long open_ring() waits before asking again after Error Unavailable.
constexpr Milliseconds kRingRetryInterval{10};
/// How often the status block is polled for the ring's slot.
constexpr Milliseconds kAckPollInterval{1};

Error make_error(ErrorKind kind, std::string text) {
    return Error{.kind = kind, .code = ipc::ErrorCode::BadMessage, .text = std::move(text)};
}

/// Sleeps for `interval`, but not past `deadline`.
void sleep_until_next(Milliseconds interval, Clock::time_point deadline) {
    std::this_thread::sleep_until(std::min(Clock::now() + interval, deadline));
}

Error closed_error() {
    return make_error(ErrorKind::Closed, "closed");
}

/// What a connection failure means for the session.
Error connection_error(const ipc::ConnectionFailure& failure) {
    switch (failure.error) {
    case ipc::ConnectionError::Closed:
        return make_error(ErrorKind::Closed, "kool_spin closed the connection");
    case ipc::ConnectionError::Truncated:
        return make_error(ErrorKind::Closed, "kool_spin closed the connection mid-frame");
    case ipc::ConnectionError::System:
        return make_error(ErrorKind::Closed, "connection failed: " + failure.system.message());
    case ipc::ConnectionError::BadFrame:
        return make_error(ErrorKind::Protocol, "a frame was refused");
    case ipc::ConnectionError::QueueFull:
    case ipc::ConnectionError::TooManyFds:
        break;
    }
    return make_error(ErrorKind::Protocol, "a frame couldn't be queued");
}

std::string map_error_text(const ipc::MapFailure& failure) {
    switch (failure.error) {
    case ipc::MapError::System:
        return "can't map the segment: " + failure.system.message();
    case ipc::MapError::NotSealed:
        return "the segment isn't sealed";
    case ipc::MapError::TooSmall:
        return "the segment is too small";
    case ipc::MapError::BadMagic:
        return "the segment isn't a kool_spin segment";
    case ipc::MapError::BadVersion:
        return "the segment has another layout version";
    case ipc::MapError::BadKind:
        return "the segment is of another kind";
    case ipc::MapError::BadSize:
        return "the segment's size doesn't match its header";
    }
    return "the segment was refused";
}

/// Milliseconds left until `deadline`, for poll(): 0 once it has passed.
int poll_timeout(Clock::time_point deadline) {
    const Clock::time_point now = Clock::now();
    if (now >= deadline) {
        return 0;
    }
    // Rounded up, so that poll() doesn't return just before the deadline.
    const auto left = std::chrono::ceil<Milliseconds>(deadline - now).count();
    return static_cast<int>(std::min<decltype(left)>(left, 60'000));
}

/// Sets O_NONBLOCK on `fd`. Returns false, with errno set, if it fails.
bool set_nonblocking(int fd) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl() is variadic
    const int flags = ::fcntl(fd, F_GETFL);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl() is variadic
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

/// A message as received, with its descriptors.
struct Incoming {
    ipc::Message message;
    std::vector<ipc::Fd> fds;
};

} // namespace

struct Session::Impl {
    std::optional<ipc::Connection> connection;
    /// The engine-state segment, mapped read-only.
    std::optional<ipc::Region> state_region;
    /// Inside state_region's own mapping.
    ipc::EngineState* state = nullptr;
    /// Once set, the session is over, and every call returns it.
    std::optional<Error> failed;
    /// Once set, the connection is lost, and every request returns it. The
    /// ring and the state stay mapped.
    std::optional<Error> lost;
    std::uint32_t next_request_id = 1;
    /// DecodeEnded notifications not yet taken (at most 64).
    DecodeQueue pending;
    /// The command-ring segment, mapped read-write, once open_ring() has
    /// succeeded.
    std::optional<ipc::Region> ring_region;
    /// Writes into ring_region's own mapping.
    std::optional<Producer> producer;
    /// The ring's client slot, below kStateClients.
    std::uint32_t slot = 0;
    std::uint64_t next_sequence = 1;
    /// The slot's rejected count as of the last wait_applied() (or
    /// open_ring()).
    std::uint64_t rejected_seen = 0;

    /// The error every call on `impl` returns, if it can't be used: a
    /// moved-from session, or one that is closed or has failed.
    static std::optional<Error> unusable(const Impl* impl) {
        if (impl == nullptr) {
            return closed_error();
        }
        return impl->failed;
    }

    /// The error every request on `impl` returns, if it can make none:
    /// unusable(), or the connection is lost.
    static std::optional<Error> unreachable(const Impl* impl) {
        const std::optional<Error> over = unusable(impl);
        return over ? over : impl->lost;
    }

    /// What a request returns when there's no connection.
    [[nodiscard]] Error no_connection() const {
        return failed.value_or(lost.value_or(closed_error()));
    }

    /// Ends what `error` ends, and returns it: the connection alone if it's
    /// Closed, and otherwise the session, with its mappings.
    Error fail(Error error) {
        if (error.kind == ErrorKind::Closed) {
            connection.reset();
            lost = error;
            return error;
        }
        drop();
        failed = error;
        return error;
    }

    /// Drops the connection and the mappings.
    void drop() noexcept {
        connection.reset();
        producer.reset();
        ring_region.reset();
        state_region.reset();
        state = nullptr;
    }

    /// A copy of this session's slot from the status block, read through
    /// the seqlock. BadState if the block stays unstable for 10 ms or holds
    /// an invalid rejection code.
    [[nodiscard]] Result<ipc::ClientAck> read_ack() const {
        if (state == nullptr) {
            return std::unexpected{failed.value_or(closed_error())};
        }
        const ipc::SeqlockReader<ipc::EngineStatus> reader{state->status};
        const Clock::time_point deadline = Clock::now() + kSnapshotBudget;
        std::optional<ipc::EngineStatus> status = reader.try_read();
        while (!status.has_value() && Clock::now() < deadline) {
            std::this_thread::yield();
            status = reader.try_read();
        }
        if (!status.has_value()) {
            return std::unexpected{make_error(ErrorKind::BadState, "unstable seqlock: status")};
        }
        // slot was checked against kStateClients when the ring was opened.
        const ipc::ClientAck ack = std::span{status->clients}[slot];
        if (ack.last_rejection > static_cast<std::uint64_t>(Rejection::BadValue)) {
            return std::unexpected{make_error(
                ErrorKind::BadState, "invalid client " + std::to_string(slot) +
                                         " last_rejection: " + std::to_string(ack.last_rejection))};
        }
        return ack;
    }

    /// Asks for a ring (again every 10 ms while the answer is Unavailable),
    /// maps it, and waits until the status block shows its slot attached.
    Result<std::uint32_t> open_ring(Clock::time_point deadline) {
        std::vector<ipc::Fd> fds;
        Result<ipc::msg::RingOpened> opened =
            request<ipc::msg::RingOpened>(ipc::msg::OpenRing{}, deadline, &fds);
        while (!opened && opened.error().kind == ErrorKind::Refused &&
               opened.error().code == ipc::ErrorCode::Unavailable && Clock::now() < deadline) {
            sleep_until_next(kRingRetryInterval, deadline);
            opened = request<ipc::msg::RingOpened>(ipc::msg::OpenRing{}, deadline, &fds);
        }
        if (!opened) {
            return std::unexpected{std::move(opened.error())};
        }
        if (fds.size() != 1 || opened->slot >= ipc::kStateClients) {
            return std::unexpected{
                fail(make_error(ErrorKind::Protocol, "RingOpened needs one descriptor and a "
                                                     "slot below " +
                                                         std::to_string(ipc::kStateClients)))};
        }
        std::expected<ipc::Region, ipc::MapFailure> region =
            ipc::Region::map(fds.front().get(), ipc::SegmentKind::CommandRing,
                             ipc::kCommandRingLayoutVersion, ipc::Access::ReadWrite);
        if (!region) {
            return std::unexpected{
                make_error(ErrorKind::BadSegment, map_error_text(region.error()))};
        }
        auto* const ring = region->at<ipc::CommandRing>(ipc::kSegmentHeaderSize);
        if (ring == nullptr) {
            return std::unexpected{
                make_error(ErrorKind::BadSegment, "the segment is too small for a command ring")};
        }
        slot = opened->slot;
        ring_region = std::move(*region);
        producer.emplace(*ring);

        // Until the engine attaches the ring, the slot may still show its
        // earlier occupant. A fresh ring has read nothing, and nothing is
        // sent before this returns, so wait for an attached slot that has
        // read no commands.
        while (true) {
            Result<ipc::ClientAck> ack = read_ack();
            if (!ack) {
                return std::unexpected{std::move(ack.error())};
            }
            if ((ack->flags & ipc::kClientAttached) != 0 && ack->commands_read == 0) {
                rejected_seen = ack->rejected;
                return slot;
            }
            if (Clock::now() >= deadline) {
                return std::unexpected{
                    make_error(ErrorKind::Timeout, "the engine didn't show slot " +
                                                       std::to_string(slot) + " attached in time")};
            }
            sleep_until_next(kAckPollInterval, deadline);
        }
    }

    /// True if the engine has taken the command with `sequence` from this
    /// session's own ring. Pushes and sequence numbers go up together, so the
    /// commands not yet taken are the last ones sent.
    bool left_ring(std::uint64_t sequence) {
        const std::optional<std::uint64_t> unread = producer ? producer->unread() : std::nullopt;
        return unread && sequence + *unread < next_sequence;
    }

    /// Waits until the slot has read `sequence` (see Session::wait_applied()).
    Result<Applied> wait_applied(std::uint64_t sequence, Clock::time_point deadline) {
        while (true) {
            Result<ipc::ClientAck> ack = read_ack();
            if (!ack) {
                return std::unexpected{std::move(ack.error())};
            }
            if ((ack->flags & ipc::kClientCorrupt) != 0) {
                return Applied{.outcome = Outcome::Corrupt};
            }
            if (ack->last_sequence >= sequence && (connection || left_ring(sequence))) {
                if (ack->rejected > rejected_seen) {
                    rejected_seen = ack->rejected;
                    return Applied{.outcome = Outcome::Rejected,
                                   .rejection = static_cast<Rejection>(ack->last_rejection)};
                }
                return Applied{.outcome = Outcome::Applied};
            }
            if (Clock::now() >= deadline) {
                return Applied{.outcome = Outcome::TimedOut};
            }
            sleep_until_next(kAckPollInterval, deadline);
        }
    }

    /// Queues `message` and writes it, waiting for the socket until
    /// `deadline`.
    std::expected<void, Error> send(const ipc::Message& message, Clock::time_point deadline) {
        const std::expected<ipc::Frame, ipc::MessageError> frame = ipc::encode(message);
        if (!frame) {
            return std::unexpected{
                make_error(ErrorKind::InvalidArgument, "can't encode the message")};
        }
        if (!connection) {
            return std::unexpected{no_connection()};
        }
        // Every path that fail()s, which drops the connection, returns at
        // once.
        ipc::Connection& open = *connection;
        if (const auto sent = open.send(*frame, {}); !sent) {
            return std::unexpected{fail(connection_error(sent.error()))};
        }
        while (true) {
            if (const auto flushed = open.flush(); !flushed) {
                return std::unexpected{fail(make_error(
                    ErrorKind::Closed, "connection failed: " + flushed.error().message()))};
            }
            if (!open.has_queued()) {
                return {};
            }
            pollfd ready{.fd = open.socket(), .events = POLLOUT, .revents = 0};
            const int polled = ::poll(&ready, 1, poll_timeout(deadline));
            if (polled < 0 && errno != EINTR) {
                return std::unexpected{fail(make_error(ErrorKind::Closed, "poll() failed"))};
            }
            if (polled == 0 && Clock::now() >= deadline) {
                return std::unexpected{make_error(ErrorKind::Timeout, "timed out sending")};
            }
        }
    }

    /// The next message, waiting no later than `deadline`: nothing if none
    /// came in time. A frame or message refused ends the session.
    std::expected<std::optional<Incoming>, Error> receive_until(Clock::time_point deadline) {
        if (!connection) {
            return std::unexpected{no_connection()};
        }
        // Every path that fail()s, which drops the connection, returns at
        // once.
        ipc::Connection& open = *connection;
        while (true) {
            // A frame may be complete already, or arrive in pieces: receive()
            // on the non-blocking socket takes whatever there is.
            auto received = open.receive();
            if (!received) {
                return std::unexpected{fail(connection_error(received.error()))};
            }
            if (*received) {
                std::expected<ipc::Message, ipc::MessageError> message =
                    ipc::decode((*received)->frame);
                if (!message) {
                    return std::unexpected{
                        fail(make_error(ErrorKind::Protocol, "a message was refused"))};
                }
                return Incoming{.message = std::move(*message), .fds = std::move((*received)->fds)};
            }
            if (Clock::now() >= deadline) {
                return std::optional<Incoming>{};
            }
            pollfd ready{.fd = open.socket(), .events = POLLIN, .revents = 0};
            const int polled = ::poll(&ready, 1, poll_timeout(deadline));
            if (polled < 0 && errno != EINTR) {
                return std::unexpected{fail(make_error(ErrorKind::Closed, "poll() failed"))};
            }
        }
    }

    /// Sends `body` with the next request ID and waits until `deadline` for
    /// the reply of type `Reply` with that ID. An Error with that ID is
    /// Refused; no reply in time is Timeout. Notifications that arrive
    /// meanwhile are queued, and every other message is ignored. The reply's
    /// descriptors go to `fds`, if given.
    template <typename Reply, typename Body>
    std::expected<Reply, Error> request(Body body, Clock::time_point deadline,
                                        std::vector<ipc::Fd>* fds = nullptr) {
        const std::uint32_t id = next_request_id++;
        if (auto sent = send(ipc::Message{.request_id = id, .body = std::move(body)}, deadline);
            !sent) {
            return std::unexpected{sent.error()};
        }
        while (true) {
            auto incoming = receive_until(deadline);
            if (!incoming) {
                return std::unexpected{incoming.error()};
            }
            if (!*incoming) {
                return std::unexpected{make_error(ErrorKind::Timeout, "no reply in time")};
            }
            ipc::Message& got = (*incoming)->message;
            if (got.request_id != id) {
                pending.offer(got);
                continue;
            }
            if (auto* const error = std::get_if<ipc::msg::Error>(&got.body)) {
                return std::unexpected{
                    Error{.kind = ErrorKind::Refused, .code = error->code, .text = error->text}};
            }
            if (auto* const reply = std::get_if<Reply>(&got.body)) {
                if (fds != nullptr) {
                    *fds = std::move((*incoming)->fds);
                }
                return std::move(*reply);
            }
            // Not the reply this request waits for: ignored.
        }
    }

    /// The first queued DecodeEnded, or the next to arrive by `deadline`.
    std::expected<ipc::msg::DecodeEnded, Error> next_decode_ended(Clock::time_point deadline) {
        while (true) {
            if (std::optional<ipc::msg::DecodeEnded> first = pending.take()) {
                return *first;
            }
            auto incoming = receive_until(deadline);
            if (!incoming) {
                return std::unexpected{incoming.error()};
            }
            if (!*incoming) {
                return std::unexpected{make_error(ErrorKind::Timeout, "no DecodeEnded in time")};
            }
            pending.offer((*incoming)->message);
        }
    }

    /// Says Hello as `name` and waits for Welcome, then maps the engine-state
    /// segment.
    std::expected<void, Error> hello(std::string_view name, Clock::time_point deadline) {
        const std::uint32_t id = next_request_id++;
        if (auto sent =
                send(ipc::Message{.request_id = id, .body = ipc::msg::Hello{std::string{name}}},
                     deadline);
            !sent) {
            return sent;
        }
        while (true) {
            auto incoming = receive_until(deadline);
            if (!incoming) {
                return std::unexpected{incoming.error()};
            }
            if (!*incoming) {
                return std::unexpected{fail(make_error(ErrorKind::Timeout, "no Welcome in time"))};
            }
            Incoming& got = **incoming;
            if (got.message.request_id != id) {
                continue; // Not an answer to the Hello.
            }
            if (const auto* const error = std::get_if<ipc::msg::Error>(&got.message.body)) {
                return std::unexpected{fail(
                    Error{.kind = ErrorKind::Refused, .code = error->code, .text = error->text})};
            }
            if (!std::holds_alternative<ipc::msg::Welcome>(got.message.body) ||
                got.fds.size() != 1) {
                continue; // Not a reply to the Hello.
            }
            std::expected<ipc::Region, ipc::MapFailure> region =
                ipc::Region::map(got.fds.front().get(), ipc::SegmentKind::EngineState,
                                 ipc::kEngineStateLayoutVersion, ipc::Access::ReadOnly);
            if (!region) {
                return std::unexpected{
                    fail(make_error(ErrorKind::BadSegment, map_error_text(region.error())))};
            }
            auto* const mapped = region->at<ipc::EngineState>(ipc::kSegmentHeaderSize);
            if (mapped == nullptr) {
                return std::unexpected{fail(make_error(
                    ErrorKind::BadSegment, "the segment is too small for the engine's state"))};
            }
            state_region = std::move(*region);
            state = mapped;
            return {};
        }
    }
};

Result<Session> Session::connect(std::string_view name, const std::filesystem::path& runtime_dir,
                                 Milliseconds timeout) {
    return connect_as(name, runtime_dir, ::geteuid(), timeout);
}

Result<Session> Session::connect_as(std::string_view name, const std::filesystem::path& runtime_dir,
                                    uid_t expected_uid, Milliseconds timeout) {
    const Clock::time_point deadline = Clock::now() + timeout;
    if (name.empty() || name.size() > kMaxNameSize) {
        return std::unexpected{make_error(ErrorKind::InvalidArgument, "a name has 1 to 64 bytes")};
    }

    std::expected<ipc::Fd, std::error_code> socket =
        ipc::connect_to_supervisor(runtime_dir, expected_uid);
    if (!socket) {
        return std::unexpected{make_error(ErrorKind::NotReachable, socket.error().message())};
    }
    // Non-blocking, so that no wait goes past its deadline, even in the
    // middle of a frame.
    if (!set_nonblocking(socket->get())) {
        return std::unexpected{make_error(
            ErrorKind::NotReachable, std::error_code{errno, std::generic_category()}.message())};
    }

    auto impl = std::make_unique<Impl>();
    impl->connection.emplace(std::move(*socket));
    if (auto said = impl->hello(name, deadline); !said) {
        return std::unexpected{said.error()};
    }
    return Session{std::move(impl)};
}

Session::Session(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;
Session::~Session() = default;

Result<std::uint32_t> Session::open_ring(Milliseconds timeout) {
    const Clock::time_point deadline = Clock::now() + timeout;
    if (std::optional<Error> error = Impl::unreachable(impl_.get())) {
        return std::unexpected{std::move(*error)};
    }
    if (impl_->producer) {
        return std::unexpected{
            make_error(ErrorKind::RingAlreadyOpen, "the session has a ring already")};
    }
    return impl_->open_ring(deadline);
}

Result<std::uint64_t> Session::send(const Command& command) {
    if (std::optional<Error> error = Impl::unusable(impl_.get())) {
        return std::unexpected{std::move(*error)};
    }
    if (!impl_->producer) {
        return std::unexpected{make_error(ErrorKind::NoRing, "the session has no ring")};
    }
    Producer& producer = *impl_->producer;
    if (producer.corrupt()) {
        return std::unexpected{make_error(ErrorKind::RingUnavailable, "the ring is corrupt")};
    }
    const std::uint64_t sequence = impl_->next_sequence;
    const ipc::CommandSlot slot{.sequence = sequence,
                                .type = static_cast<std::uint32_t>(command.type),
                                .target = command.target,
                                .payload = command.payload};
    if (!producer.try_push(slot)) {
        return std::unexpected{make_error(ErrorKind::RingUnavailable, producer.corrupt()
                                                                          ? "the ring is corrupt"
                                                                          : "the ring is full")};
    }
    ++impl_->next_sequence;
    return sequence;
}

Result<Applied> Session::wait_applied(std::uint64_t sequence, Milliseconds timeout) {
    const Clock::time_point deadline = Clock::now() + timeout;
    if (std::optional<Error> error = Impl::unusable(impl_.get())) {
        return std::unexpected{std::move(*error)};
    }
    if (!impl_->producer) {
        return std::unexpected{make_error(ErrorKind::NoRing, "the session has no ring")};
    }
    return impl_->wait_applied(sequence, deadline);
}

Result<Snapshot> Session::snapshot() const {
    if (std::optional<Error> error = Impl::unusable(impl_.get())) {
        return std::unexpected{std::move(*error)};
    }
    if (impl_->state == nullptr) {
        return std::unexpected{closed_error()};
    }
    return snapshot_from(*impl_->state, kSnapshotBudget);
}

Result<LoadedFile> Session::load_file(std::uint32_t deck, const std::string& path,
                                      Milliseconds timeout) {
    const Clock::time_point deadline = Clock::now() + timeout;
    if (std::optional<Error> error = Impl::unreachable(impl_.get())) {
        return std::unexpected{std::move(*error)};
    }
    auto loaded = impl_->request<ipc::msg::FileLoaded>(
        ipc::msg::LoadFile{.deck = deck, .path = path}, deadline);
    if (!loaded) {
        return std::unexpected{std::move(loaded.error())};
    }
    return LoadedFile{.track_id = loaded->track_id,
                      .sample_rate = loaded->sample_rate,
                      .estimated_frames = loaded->estimated_frames};
}

Result<void> Session::unload_deck(std::uint32_t deck, Milliseconds timeout) {
    const Clock::time_point deadline = Clock::now() + timeout;
    if (std::optional<Error> error = Impl::unreachable(impl_.get())) {
        return std::unexpected{std::move(*error)};
    }
    auto unloaded =
        impl_->request<ipc::msg::DeckUnloaded>(ipc::msg::UnloadDeck{.deck = deck}, deadline);
    if (!unloaded) {
        return std::unexpected{std::move(unloaded.error())};
    }
    return {};
}

Result<void> Session::stop(Milliseconds timeout) {
    const Clock::time_point deadline = Clock::now() + timeout;
    if (std::optional<Error> error = Impl::unreachable(impl_.get())) {
        return std::unexpected{std::move(*error)};
    }
    auto done = impl_->request<ipc::msg::Done>(ipc::msg::Stop{}, deadline);
    if (!done) {
        return std::unexpected{std::move(done.error())};
    }
    // kool_spin has stopped: the session is over.
    close();
    return {};
}

Result<void> Session::ping(Milliseconds timeout) {
    const Clock::time_point deadline = Clock::now() + timeout;
    if (std::optional<Error> error = Impl::unreachable(impl_.get())) {
        return std::unexpected{std::move(*error)};
    }
    auto pong = impl_->request<ipc::msg::Pong>(ipc::msg::Ping{}, deadline);
    if (!pong) {
        return std::unexpected{std::move(pong.error())};
    }
    return {};
}

Result<ipc::msg::DecodeEnded> Session::next_decode_ended(Milliseconds timeout) {
    const Clock::time_point deadline = Clock::now() + timeout;
    if (std::optional<Error> error = Impl::unusable(impl_.get())) {
        return std::unexpected{std::move(*error)};
    }
    return impl_->next_decode_ended(deadline);
}

bool Session::connected() const noexcept {
    return impl_ && impl_->connection.has_value();
}

void Session::close() noexcept {
    if (impl_) {
        impl_->drop();
        // Kept short, so the copy fits the string's own buffer and can't
        // throw.
        impl_->failed =
            Error{.kind = ErrorKind::Closed, .code = ipc::ErrorCode::BadMessage, .text = "closed"};
    }
}

} // namespace ks::client
