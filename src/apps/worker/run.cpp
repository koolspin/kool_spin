// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/worker_app/run.h>

#include <ks/decode/decoder.h>
#include <ks/ipc/child_process.h>
#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/frame.h>
#include <ks/ipc/log.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/region.h>
#include <ks/ipc/track_segment.h>
#include <ks/worker_app/options.h>

#include <poll.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace ks::worker_app {

namespace {

using Clock = std::chrono::steady_clock;
namespace msg = ipc::msg;

/// What ended a wait for the supervisor's next message.
enum class Ended : std::uint8_t {
    Signal, ///< A stop signal.
    Closed, ///< The socket closed, or failed, or a frame was refused.
};

/// A message from the supervisor, with its descriptors.
struct Received {
    ipc::Message message;
    std::vector<ipc::Fd> fds;
};

/// `frames` at `rate` as seconds.
double seconds(std::uint64_t frames, std::uint32_t rate) {
    return static_cast<double>(frames) / static_cast<double>(rate);
}

/// The decode worker: one job, from the supervisor's Probe to its Decoded
/// reply (specs/decode-worker/spec.md requirements 17 to 20).
class WorkerProcess {
public:
    WorkerProcess(const Options& options, std::ostream& err) : options_{&options}, err_{&err} {}

    int run() {
        if (const std::expected<void, std::string> started = start(); !started) {
            // Starting failures go to standard error, and to the log if it's
            // open (specs/logs/spec.md requirement 18).
            if (log_) {
                log_->write(ipc::LogLevel::Error, started.error());
            }
            *err_ << kPrefix << started.error() << '\n' << std::flush;
            return kExitStartFailed;
        }
        return job();
    }

private:
    static constexpr std::string_view kPrefix = "ks-worker: ";

    /// Where the job has got to.
    enum class Stage : std::uint8_t { WaitingForProbe, WaitingForDecode };

    /// One report: a line in the log, or on standard error without --log-fd.
    void report(ipc::LogLevel level, std::string_view line) {
        if (log_) {
            log_->write(level, line);
        } else {
            *err_ << kPrefix << line << '\n' << std::flush;
        }
    }

    std::expected<void, std::string> start();
    int job();

    /// Waits for the supervisor's next message, answering Pings and Errors
    /// itself. Nothing if a stop signal or the socket closing ended the wait;
    /// `ended_` says which.
    std::optional<Received> next_message();

    /// Reads whatever the supervisor has sent, without waiting. A Ping is
    /// answered, an Error reported, and anything else refused as
    /// NotSupported: while it decodes, the worker takes nothing else. False
    /// once the socket has closed.
    bool read_while_decoding();

    /// The next message on the socket, without waiting; Pings and Errors are
    /// handled on the way. Nothing if there's none yet, or if the socket has
    /// closed, which sets `closed`. A refused frame, or one that doesn't
    /// decode, is answered with BadMessage and closes the socket.
    std::optional<Received> receive_one(bool& closed);

    void send(const ipc::Message& message);
    void not_supported(std::uint32_t id, std::string_view what);
    /// Refuses `id` with `text`, and logs it.
    void refuse(std::uint32_t id, const std::string& text);
    /// Closes the supervisor's socket, saying why in the log.
    void lose_supervisor(std::string_view why);

    /// Probes the file. True if it can be decoded.
    bool probe(std::uint32_t id, std::vector<ipc::Fd>& fds);
    /// Decodes into the segment, replies, and returns the exit status.
    int decode(std::uint32_t id, std::uint64_t max_frames, std::vector<ipc::Fd>& fds);

    const Options* options_;
    std::ostream* err_;

    ipc::Fd log_fd_;
    std::unique_ptr<ipc::Region> log_region_;
    std::unique_ptr<ipc::LogWriter> log_;
    std::unique_ptr<ipc::Connection> supervisor_;

    Ended ended_ = Ended::Closed;
    std::optional<decode::Decoder> decoder_;
    /// A stop signal arrived while decoding.
    bool stopped_ = false;
};

std::expected<void, std::string> WorkerProcess::start() {
    // Before anything else, so that no signal can end it by its default
    // action once it's running.
    if (const std::error_code blocked = ipc::block_stop_signals(); blocked) {
        return std::unexpected{"can't block signals: " + blocked.message()};
    }
    const int supervisor_fd = options_->supervisor_fd.value_or(-1);
    if (!ipc::adopt_socket(supervisor_fd)) {
        return std::unexpected{
            std::format("--supervisor-fd {} isn't a stream socket", supervisor_fd)};
    }
    supervisor_ = std::make_unique<ipc::Connection>(ipc::Fd{supervisor_fd});
    if (options_->log_fd) {
        const int log_fd = *options_->log_fd;
        if (!ipc::adopt_descriptor(log_fd)) {
            return std::unexpected{std::format("--log-fd {} isn't open", log_fd)};
        }
        log_fd_ = ipc::Fd{log_fd};
        std::expected<ipc::Region, ipc::MapFailure> region = ipc::Region::map(
            log_fd_.get(), ipc::SegmentKind::Log, ipc::kLogLayoutVersion, ipc::Access::ReadWrite);
        if (!region) {
            return std::unexpected{std::string{"--log-fd isn't a log segment"}};
        }
        log_region_ = std::make_unique<ipc::Region>(std::move(*region));
        // at() checks against the size of the worker's own mapping.
        auto* const segment = log_region_->at<ipc::LogSegment>(ipc::kSegmentHeaderSize);
        if (segment == nullptr) {
            return std::unexpected{std::string{"the log segment is too small"}};
        }
        log_ = std::make_unique<ipc::LogWriter>(*segment);
    }
    return {};
}

int WorkerProcess::job() {
    Stage stage = Stage::WaitingForProbe;
    for (;;) {
        std::optional<Received> received = next_message();
        if (!received) {
            report(ipc::LogLevel::Warning, ended_ == Ended::Signal
                                               ? "stopped by a signal before decoding"
                                               : "the supervisor's socket closed before decoding");
            return kExitOk;
        }
        const ipc::Message& message = received->message;
        const std::uint32_t id = message.request_id;
        if (std::holds_alternative<msg::Probe>(message.body)) {
            if (stage != Stage::WaitingForProbe) {
                not_supported(id, "a second Probe");
                continue;
            }
            if (!probe(id, received->fds)) {
                return kExitOk;
            }
            stage = Stage::WaitingForDecode;
        } else if (const auto* const decode_asked = std::get_if<msg::Decode>(&message.body)) {
            if (stage != Stage::WaitingForDecode) {
                not_supported(id, "a Decode before a Probe");
                continue;
            }
            return decode(id, decode_asked->max_frames, received->fds);
        } else {
            not_supported(id, "not for a worker");
        }
    }
}

std::optional<Received> WorkerProcess::next_message() {
    for (;;) {
        if (!supervisor_) {
            ended_ = Ended::Closed;
            return std::nullopt;
        }
        // Signals don't wake the poll; they wait for the next pass, at most
        // kWaitInterval away.
        pollfd polled{.fd = supervisor_->socket(),
                      .events =
                          static_cast<short>(supervisor_->has_queued() ? POLLIN | POLLOUT : POLLIN),
                      .revents = 0};
        static_cast<void>(::poll(&polled, 1, static_cast<int>(kWaitInterval.count())));
        if (ipc::take_stop_signal()) {
            ended_ = Ended::Signal;
            return std::nullopt;
        }
        if ((polled.revents & POLLOUT) != 0 && !supervisor_->flush()) {
            lose_supervisor("couldn't be written to");
            continue;
        }
        if (polled.revents == 0) {
            continue;
        }
        bool closed = false;
        if (std::optional<Received> received = receive_one(closed)) {
            return received;
        }
    }
}

std::optional<Received> WorkerProcess::receive_one(bool& closed) {
    closed = false;
    for (;;) {
        if (!supervisor_) {
            closed = true;
            return std::nullopt;
        }
        std::expected<std::optional<ipc::ReceivedFrame>, ipc::ConnectionFailure> received =
            supervisor_->receive();
        if (!received) {
            if (received.error().error == ipc::ConnectionError::BadFrame) {
                send(ipc::Message{
                    .request_id = 0,
                    .body = msg::Error{.code = ipc::ErrorCode::BadMessage, .text = "a bad frame"}});
                lose_supervisor("sent a bad frame");
            } else {
                lose_supervisor("closed");
            }
            closed = true;
            return std::nullopt;
        }
        std::optional<ipc::ReceivedFrame>& frame = *received;
        if (!frame) {
            // Nothing more for now.
            return std::nullopt;
        }
        std::expected<ipc::Message, ipc::MessageError> message = ipc::decode(frame->frame);
        if (!message) {
            send(ipc::Message{.request_id = frame->frame.request_id,
                              .body = msg::Error{.code = ipc::ErrorCode::BadMessage,
                                                 .text = "a message that doesn't decode"}});
            lose_supervisor("sent a message that doesn't decode");
            closed = true;
            return std::nullopt;
        }
        if (std::holds_alternative<msg::Ping>(message->body)) {
            send(ipc::Message{.request_id = message->request_id, .body = msg::Pong{}});
            continue;
        }
        if (const auto* const error = std::get_if<msg::Error>(&message->body)) {
            report(ipc::LogLevel::Warning, "the supervisor reported an error: " + error->text);
            continue;
        }
        return Received{.message = std::move(*message), .fds = std::move(frame->fds)};
    }
}

bool WorkerProcess::read_while_decoding() {
    for (;;) {
        bool closed = false;
        const std::optional<Received> received = receive_one(closed);
        if (closed) {
            return false;
        }
        if (!received) {
            return true;
        }
        not_supported(received->message.request_id, "while it decodes");
    }
}

void WorkerProcess::send(const ipc::Message& message) {
    if (!supervisor_) {
        return;
    }
    const std::expected<ipc::Frame, ipc::MessageError> frame = ipc::encode(message);
    if (!frame || !supervisor_->send(*frame, {}) || !supervisor_->flush()) {
        lose_supervisor("couldn't be written to");
    }
}

void WorkerProcess::not_supported(std::uint32_t id, std::string_view what) {
    send(ipc::Message{
        .request_id = id,
        .body = msg::Error{.code = ipc::ErrorCode::NotSupported, .text = std::string{what}}});
}

void WorkerProcess::refuse(std::uint32_t id, const std::string& text) {
    report(ipc::LogLevel::Warning, "refused: " + text);
    send(ipc::Message{.request_id = id,
                      .body = msg::Error{.code = ipc::ErrorCode::Refused, .text = text}});
}

void WorkerProcess::lose_supervisor(std::string_view why) {
    if (!supervisor_) {
        return;
    }
    report(ipc::LogLevel::Warning, std::format("the supervisor's socket {}", why));
    supervisor_.reset();
}

bool WorkerProcess::probe(std::uint32_t id, std::vector<ipc::Fd>& fds) {
    // decode() checked the count; the connection delivers what it says.
    if (fds.size() != 1) {
        refuse(id, "a Probe without its file");
        return false;
    }
    std::expected<decode::Decoder, decode::Refusal> decoder =
        decode::Decoder::open(std::make_unique<decode::FdSource>(std::move(fds.front())));
    if (!decoder) {
        refuse(id, decoder.error().text);
        return false;
    }
    const decode::DecoderInfo& info = decoder->info();
    report(ipc::LogLevel::Info,
           std::format("probed: {}, {} Hz, {} channels, {:.3f} s", info.codec, info.sample_rate,
                       info.channels, seconds(info.estimated_frames, info.sample_rate)));
    send(ipc::Message{.request_id = id,
                      .body = msg::Probed{.sample_rate = info.sample_rate,
                                          .channels = info.channels,
                                          .estimated_frames = info.estimated_frames}});
    decoder_ = std::move(*decoder);
    return true;
}

int WorkerProcess::decode(std::uint32_t id, std::uint64_t max_frames, std::vector<ipc::Fd>& fds) {
    if (fds.size() != 1) {
        refuse(id, "a Decode without its segment");
        return kExitOk;
    }
    std::expected<ipc::Region, ipc::MapFailure> region =
        ipc::Region::map(fds.front().get(), ipc::SegmentKind::Track, ipc::kTrackLayoutVersion,
                         ipc::Access::ReadWrite);
    if (!region) {
        refuse(id, "the segment isn't a track segment");
        return kExitOk;
    }
    ipc::TrackWriter writer{region->bytes()};
    if (writer.capacity() < max_frames) {
        refuse(id, std::format("the segment holds {} frames, fewer than the {} asked for",
                               writer.capacity(), max_frames));
        return kExitOk;
    }

    if (!decoder_) {
        // job() asks for a Decode only after a Probe that opened a decoder.
        refuse(id, "no file was probed");
        return kExitOk;
    }
    decode::Decoder& decoder = *decoder_;
    const std::uint32_t rate = decoder.info().sample_rate;
    const Clock::time_point began = Clock::now();
    bool reported_closed = false;
    const decode::DecodedTrack decoded = decode::decode_track(decoder, writer, max_frames, [&] {
        if (ipc::take_stop_signal()) {
            stopped_ = true;
            return true;
        }
        // Without the supervisor, it decodes on to the end, so a track the
        // engine plays gets every frame (ADR 0003 §12).
        if (supervisor_ && !read_while_decoding() && !reported_closed) {
            reported_closed = true;
            report(ipc::LogLevel::Warning, "decoding on without the supervisor");
        }
        return false;
    });
    const double took = std::chrono::duration<double>(Clock::now() - began).count();

    if (decoder.skipped_packets() > 0) {
        report(ipc::LogLevel::Warning,
               std::format("skipped {} packets that didn't decode", decoder.skipped_packets()));
    }
    switch (decoded.status) {
    case ipc::TrackStatus::Complete:
        report(ipc::LogLevel::Info,
               std::format("decoded {} frames ({:.3f} s) in {:.3f} s", decoded.frames,
                           seconds(decoded.frames, rate), took));
        break;
    case ipc::TrackStatus::Truncated:
        report(ipc::LogLevel::Warning,
               std::format("truncated at {} frames ({:.3f} s): the most it was asked for",
                           decoded.frames, seconds(decoded.frames, rate)));
        break;
    case ipc::TrackStatus::Failed:
    case ipc::TrackStatus::Decoding:
        if (stopped_) {
            report(ipc::LogLevel::Warning,
                   std::format("stopped by a signal after {} frames", decoded.frames));
            // A stopped worker exits without replying.
            return kExitOk;
        }
        {
            // After an error, read() keeps returning it.
            const std::expected<std::size_t, decode::DecodeError> error = decoder.read({});
            report(ipc::LogLevel::Error,
                   std::format("failed after {} frames: {}", decoded.frames,
                               error ? std::string{"decoding stopped"} : error.error().text));
        }
        break;
    }
    send(ipc::Message{.request_id = id,
                      .body = msg::Decoded{.frames = decoded.frames, .status = decoded.status}});
    return kExitOk;
}

} // namespace

int run(const Options& options, std::ostream& out, std::ostream& err) {
    if (options.help) {
        out << usage();
        return kExitOk;
    }
    WorkerProcess worker{options, err};
    return worker.run();
}

} // namespace ks::worker_app
