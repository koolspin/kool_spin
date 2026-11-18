// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/client/session.h>

#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/frame.h>
#include <ks/ipc/listener.h>
#include <ks/ipc/region.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <deque>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

// Requests, notifications, and the ring are written by later build passes
// (specs/ks-play/plan.md): until then those calls return an error.

namespace ks::client {

namespace {

using Clock = std::chrono::steady_clock;

/// The most a snapshot() takes in all (requirement 17).
constexpr Milliseconds kSnapshotBudget{10};

Error make_error(ErrorKind kind, std::string text) {
    return Error{.kind = kind, .code = ipc::ErrorCode::BadMessage, .text = std::move(text)};
}

Error not_written() {
    return make_error(ErrorKind::Protocol, "stub");
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
    /// Once set, every call returns it.
    std::optional<Error> failed;
    std::uint32_t next_request_id = 1;
    /// DecodeEnded notifications not yet taken (at most 64).
    std::deque<ipc::msg::DecodeEnded> pending;

    /// The error every call on `impl` returns, if it can't be used: a
    /// moved-from session, or one that is closed or has failed.
    static std::optional<Error> unusable(const Impl* impl) {
        if (impl == nullptr) {
            return closed_error();
        }
        return impl->failed;
    }

    /// Ends the session with `error`, dropping the connection and the
    /// mappings, and returns it.
    Error fail(Error error) {
        drop();
        failed = error;
        return error;
    }

    /// Drops the connection and the mappings.
    void drop() noexcept {
        connection.reset();
        state_region.reset();
        state = nullptr;
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
            return std::unexpected{failed.value_or(closed_error())};
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
            return std::unexpected{failed.value_or(closed_error())};
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

Result<std::uint32_t> Session::open_ring(Milliseconds /*timeout*/) {
    if (std::optional<Error> error = Impl::unusable(impl_.get())) {
        return std::unexpected{std::move(*error)};
    }
    return std::unexpected{not_written()};
}

Result<std::uint64_t> Session::send(const Command& /*command*/) {
    if (std::optional<Error> error = Impl::unusable(impl_.get())) {
        return std::unexpected{std::move(*error)};
    }
    return std::unexpected{not_written()};
}

Result<Applied> Session::wait_applied(std::uint64_t /*sequence*/, Milliseconds /*timeout*/) {
    if (std::optional<Error> error = Impl::unusable(impl_.get())) {
        return std::unexpected{std::move(*error)};
    }
    return std::unexpected{not_written()};
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

Result<LoadedFile> Session::load_file(std::uint32_t /*deck*/, const std::string& /*path*/,
                                      Milliseconds /*timeout*/) {
    if (std::optional<Error> error = Impl::unusable(impl_.get())) {
        return std::unexpected{std::move(*error)};
    }
    return std::unexpected{not_written()};
}

Result<void> Session::unload_deck(std::uint32_t /*deck*/, Milliseconds /*timeout*/) {
    if (std::optional<Error> error = Impl::unusable(impl_.get())) {
        return std::unexpected{std::move(*error)};
    }
    return std::unexpected{not_written()};
}

Result<void> Session::stop(Milliseconds /*timeout*/) {
    if (std::optional<Error> error = Impl::unusable(impl_.get())) {
        return std::unexpected{std::move(*error)};
    }
    return std::unexpected{not_written()};
}

Result<void> Session::ping(Milliseconds /*timeout*/) {
    if (std::optional<Error> error = Impl::unusable(impl_.get())) {
        return std::unexpected{std::move(*error)};
    }
    return std::unexpected{not_written()};
}

Result<ipc::msg::DecodeEnded> Session::next_decode_ended(Milliseconds /*timeout*/) {
    if (std::optional<Error> error = Impl::unusable(impl_.get())) {
        return std::unexpected{std::move(*error)};
    }
    return std::unexpected{not_written()};
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
