// SPDX-License-Identifier: GPL-3.0-or-later
// ks_fake_worker: stands in for ks-worker in specs/track-loading/spec.md's
// acceptance tests. It takes ks-worker's command line and answers the same
// messages, but what it does comes from the file it's asked to probe: a text
// file whose first line is "ks-fake-track", then one setting per line:
//
//   rate <hz>             the rate it reports and writes (default 44100)
//   estimate <frames>     the estimate it reports (default 0: none)
//   probe-delay <ms>      how long it waits before answering Probe (default 0)
//   probe refuse <text>   answers Probe with Error Refused and <text>
//   probe hang            never answers Probe
//   decode-delay <ms>     how long it waits after Decode before it stores the
//                         rate or publishes anything (default 0)
//   frames <n>            the frames it decodes (default 0)
//   chunk <frames> <ms>   publishes them in chunks of <frames>, <ms> apart
//                         (default 4096 0)
//   end <how>             complete or failed (finishes the track so, and
//                         answers Decoded), crash (kills itself with SIGKILL
//                         without finishing the track), or hang (waits
//                         without finishing it) (default complete)
//   ignore-sigterm        never takes its stop signals, and doesn't stop
//                         when its socket closes, so only SIGKILL stops it
//                         while its supervisor runs
//
// Its frames are zero. Like ks-worker, it blocks SIGTERM, SIGINT, and SIGHUP
// and checks for them at least every 10 ms. Unless it ignores them, a stop
// signal makes it exit with status 0, finishing a track it has started as
// Failed; and its socket closing makes it exit with status 0 while it waits
// for a message, or hangs, but not while it publishes frames. A file that
// isn't a fake track is refused.
//
// Whatever the script says, it exits as soon as it finds that the process
// that started it has gone, so a test that dies, and takes kool_spin with it,
// leaves no fake worker behind.

#include <ks/ipc/child_process.h>
#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/frame.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/region.h>
#include <ks/ipc/track_segment.h>

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
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

namespace {

namespace ipc = ks::ipc;
namespace msg = ks::ipc::msg;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

/// How often it checks for a stop signal, at least.
constexpr milliseconds kTick{10};

/// The most of the file it reads.
constexpr std::size_t kMaxScriptSize = 65'536;

/// How it answers Probe.
enum class ProbeAnswer : std::uint8_t { Probed, Refuse, Hang };

/// How it ends once it has published its frames.
enum class End : std::uint8_t { Complete, Failed, Crash, Hang };

/// What the file asks for.
struct Script {
    std::uint32_t rate = 44'100;
    std::uint64_t estimate = 0;
    milliseconds probe_delay{0};
    ProbeAnswer probe = ProbeAnswer::Probed;
    std::string refusal;
    milliseconds decode_delay{0};
    std::uint64_t frames = 0;
    std::uint64_t chunk_frames = 4'096;
    milliseconds chunk_delay{0};
    End end = End::Complete;
    bool ignore_sigterm = false;
};

/// `text` as a whole decimal number, or nothing.
std::optional<std::uint64_t> number(std::string_view text) {
    std::uint64_t value = 0;
    const char* const first = std::to_address(text.begin());
    const char* const last = std::to_address(text.end());
    const auto [end, error] = std::from_chars(first, last, value);
    if (error != std::errc{} || end != last || text.empty()) {
        return std::nullopt;
    }
    return value;
}

/// Applies a setting whose value is a number, or numbers. False if `key`
/// isn't one, or its value isn't valid.
bool apply_number(Script& script, std::string_view key, std::string_view first,
                  std::string_view second) {
    const std::optional<std::uint64_t> value = number(first);
    if (!value) {
        return false;
    }
    if (key == "rate") {
        script.rate = static_cast<std::uint32_t>(*value);
    } else if (key == "estimate") {
        script.estimate = *value;
    } else if (key == "probe-delay") {
        script.probe_delay = milliseconds{*value};
    } else if (key == "decode-delay") {
        script.decode_delay = milliseconds{*value};
    } else if (key == "frames") {
        script.frames = *value;
    } else if (key == "chunk") {
        const std::optional<std::uint64_t> delay = number(second);
        if (!delay) {
            return false;
        }
        script.chunk_frames = std::max<std::uint64_t>(*value, 1);
        script.chunk_delay = milliseconds{*delay};
    } else {
        return false;
    }
    return true;
}

/// How the end setting's word ends a fake track, or nothing.
std::optional<End> end_of(std::string_view word) {
    if (word == "complete") {
        return End::Complete;
    }
    if (word == "failed") {
        return End::Failed;
    }
    if (word == "crash") {
        return End::Crash;
    }
    if (word == "hang") {
        return End::Hang;
    }
    return std::nullopt;
}

/// Applies the setting on `line`. False if it isn't one.
bool apply_setting(Script& script, const std::string& line) {
    std::istringstream words{line};
    std::string key;
    std::string first;
    std::string second;
    words >> key >> first >> second;
    if (key.empty()) {
        return true;
    }
    if (key == "probe" && first == "hang") {
        script.probe = ProbeAnswer::Hang;
        return true;
    }
    if (key == "probe" && first == "refuse") {
        // The text is everything after "probe refuse ".
        constexpr std::string_view kRefuse = "refuse";
        const std::size_t at = line.find(kRefuse) + kRefuse.size();
        script.probe = ProbeAnswer::Refuse;
        script.refusal = at < line.size() ? line.substr(at + 1) : std::string{"refused"};
        return true;
    }
    if (key == "end") {
        const std::optional<End> end = end_of(first);
        script.end = end.value_or(script.end);
        return end.has_value();
    }
    if (key == "ignore-sigterm") {
        script.ignore_sigterm = true;
        return true;
    }
    return apply_number(script, key, first, second);
}

/// The script in `text`, or nothing if it isn't a fake track.
std::optional<Script> parse(const std::string& text) {
    std::istringstream lines{text};
    std::string line;
    if (!std::getline(lines, line) || line != "ks-fake-track") {
        return std::nullopt;
    }
    Script script;
    while (std::getline(lines, line)) {
        if (!apply_setting(script, line)) {
            return std::nullopt;
        }
    }
    return script;
}

/// Up to kMaxScriptSize bytes of the file open as `fd`, from its start.
std::string read_file(int fd) {
    std::string text;
    std::array<char, 4'096> buffer{};
    for (;;) {
        const ssize_t got =
            ::pread(fd, buffer.data(), buffer.size(), static_cast<off_t>(text.size()));
        if (got == -1 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            return text;
        }
        text.append(buffer.data(), static_cast<std::size_t>(got));
        if (text.size() >= kMaxScriptSize) {
            return text;
        }
    }
}

/// A message from the supervisor, with its descriptors.
struct Received {
    ipc::Message message;
    std::vector<ipc::Fd> fds;
};

class FakeWorker {
public:
    explicit FakeWorker(ipc::Connection supervisor)
        : supervisor_{std::move(supervisor)}, parent_{::getppid()} {}

    int run() {
        // 1. The Probe.
        std::optional<Received> probe = next_of<msg::Probe>();
        if (!probe) {
            return 0;
        }
        const std::uint32_t probe_id = probe->message.request_id;
        if (probe->fds.size() != 1) {
            refuse(probe_id, "a Probe without its file");
            return 0;
        }
        const std::optional<Script> script = parse(read_file(probe->fds.front().get()));
        if (!script) {
            refuse(probe_id, "not a fake track");
            return 0;
        }
        script_ = *script;
        if (!pause(script_.probe_delay)) {
            return 0;
        }
        switch (script_.probe) {
        case ProbeAnswer::Refuse:
            refuse(probe_id, script_.refusal);
            return 0;
        case ProbeAnswer::Hang:
            // Answers nothing more; a stop signal or the socket closing ends it.
            while (next_of<msg::Decode>()) {
            }
            return 0;
        case ProbeAnswer::Probed:
            break;
        }
        send(ipc::Message{.request_id = probe_id,
                          .body = msg::Probed{.sample_rate = script_.rate,
                                              .channels = 2,
                                              .estimated_frames = script_.estimate}});

        // 2. The Decode.
        std::optional<Received> decode = next_of<msg::Decode>();
        if (!decode) {
            return 0;
        }
        const std::uint32_t decode_id = decode->message.request_id;
        const std::uint64_t max_frames = std::get<msg::Decode>(decode->message.body).max_frames;
        if (decode->fds.size() != 1) {
            refuse(decode_id, "a Decode without its segment");
            return 0;
        }
        std::expected<ipc::Region, ipc::MapFailure> region =
            ipc::Region::map(decode->fds.front().get(), ipc::SegmentKind::Track,
                             ipc::kTrackLayoutVersion, ipc::Access::ReadWrite);
        if (!region) {
            refuse(decode_id, "not a track segment");
            return 0;
        }
        if (!pause(script_.decode_delay)) {
            return 0;
        }
        return decode_into(decode_id, max_frames, region->bytes());
    }

private:
    /// Publishes the script's frames into the segment, and ends as it says.
    int decode_into(std::uint32_t id, std::uint64_t max_frames, std::span<std::byte> segment) {
        ipc::TrackWriter writer{segment};
        writer.start(script_.rate);
        const std::uint64_t frames = std::min({script_.frames, max_frames, writer.capacity()});
        const std::span<float> samples = writer.samples();
        std::uint64_t published = 0;
        while (published < frames) {
            const std::uint64_t chunk = std::min(script_.chunk_frames, frames - published);
            std::ranges::fill(samples.subspan(static_cast<std::size_t>(published) * 2,
                                              static_cast<std::size_t>(chunk) * 2),
                              0.0F);
            published += chunk;
            writer.publish(published);
            if (orphaned()) {
                return 0;
            }
            if (published < frames && !pause(script_.chunk_delay)) {
                writer.finish(ipc::TrackStatus::Failed, published);
                return 0;
            }
        }
        switch (script_.end) {
        case End::Complete:
        case End::Failed: {
            const ipc::TrackStatus status = script_.end == End::Complete
                                                ? ipc::TrackStatus::Complete
                                                : ipc::TrackStatus::Failed;
            writer.finish(status, published);
            send(ipc::Message{.request_id = id,
                              .body = msg::Decoded{.frames = published, .status = status}});
            flush();
            return 0;
        }
        case End::Crash:
            static_cast<void>(::raise(SIGKILL));
            return 1;
        case End::Hang:
            // Until a stop signal, or the socket closing.
            while (next_message()) {
            }
            writer.finish(ipc::TrackStatus::Failed, published);
            return 0;
        }
        return 1;
    }

    /// True if a stop signal has arrived and the script doesn't ignore them.
    [[nodiscard]] bool stopped() const noexcept {
        return !script_.ignore_sigterm && ipc::take_stop_signal();
    }

    /// True if the process that started it has gone.
    [[nodiscard]] bool orphaned() const noexcept { return ::getppid() != parent_; }

    /// Waits `duration`, checking for a stop signal every tick. False if one
    /// came.
    [[nodiscard]] bool pause(milliseconds duration) const {
        const Clock::time_point until = Clock::now() + duration;
        for (;;) {
            if (stopped() || orphaned()) {
                return false;
            }
            const Clock::time_point now = Clock::now();
            if (now >= until) {
                return true;
            }
            std::this_thread::sleep_for(std::min<Clock::duration>(kTick, until - now));
        }
    }

    /// Waits for a message of type `T`, answering Pings, skipping Errors, and
    /// answering anything else with NotSupported. Nothing if a stop signal
    /// came or the socket closed.
    template <typename T>
    std::optional<Received> next_of() {
        for (;;) {
            std::optional<Received> received = next_message();
            if (!received) {
                return std::nullopt;
            }
            if (std::holds_alternative<T>(received->message.body)) {
                return received;
            }
            send(ipc::Message{
                .request_id = received->message.request_id,
                .body = msg::Error{.code = ipc::ErrorCode::NotSupported, .text = "not now"}});
        }
    }

    /// The next message, other than a Ping or an Error. Nothing if a stop
    /// signal came or the socket closed.
    std::optional<Received> next_message() {
        for (;;) {
            if (stopped() || orphaned()) {
                return std::nullopt;
            }
            if (!open_) {
                if (!script_.ignore_sigterm) {
                    return std::nullopt;
                }
                // Only SIGKILL, or the supervisor going, ends it.
                std::this_thread::sleep_for(kTick);
                continue;
            }
            pollfd polled{.fd = supervisor_.socket(), .events = POLLIN, .revents = 0};
            static_cast<void>(::poll(&polled, 1, static_cast<int>(kTick.count())));
            std::expected<std::optional<ipc::ReceivedFrame>, ipc::ConnectionFailure> received =
                supervisor_.receive();
            if (!received) {
                // The loop's first check decides what a closed socket means.
                open_ = false;
                continue;
            }
            std::optional<ipc::ReceivedFrame>& frame = *received;
            if (!frame) {
                continue;
            }
            std::expected<ipc::Message, ipc::MessageError> message = ipc::decode(frame->frame);
            if (!message) {
                send(ipc::Message{.request_id = frame->frame.request_id,
                                  .body = msg::Error{.code = ipc::ErrorCode::BadMessage,
                                                     .text = "a message that doesn't decode"}});
                flush();
                open_ = false;
                continue;
            }
            if (std::holds_alternative<msg::Ping>(message->body)) {
                send(ipc::Message{.request_id = message->request_id, .body = msg::Pong{}});
                continue;
            }
            if (std::holds_alternative<msg::Error>(message->body)) {
                continue;
            }
            return Received{.message = std::move(*message), .fds = std::move(frame->fds)};
        }
    }

    void refuse(std::uint32_t id, const std::string& text) {
        send(ipc::Message{.request_id = id,
                          .body = msg::Error{.code = ipc::ErrorCode::Refused, .text = text}});
        flush();
    }

    void send(const ipc::Message& message) {
        const std::expected<ipc::Frame, ipc::MessageError> frame = ipc::encode(message);
        if (open_ && frame && (!supervisor_.send(*frame, {}) || !supervisor_.flush())) {
            open_ = false;
        }
    }

    /// Writes what's queued, for up to a second.
    void flush() {
        const Clock::time_point until = Clock::now() + std::chrono::seconds{1};
        while (open_ && supervisor_.has_queued() && Clock::now() < until) {
            pollfd polled{.fd = supervisor_.socket(), .events = POLLOUT, .revents = 0};
            static_cast<void>(::poll(&polled, 1, static_cast<int>(kTick.count())));
            if (!supervisor_.flush()) {
                return;
            }
        }
    }

    ipc::Connection supervisor_;
    pid_t parent_;
    bool open_ = true;
    Script script_;
};

/// The value after `name` in `args`, as a descriptor number, or -1.
int fd_option(std::span<char*> args, std::string_view name) {
    for (std::size_t i = 1; i + 1 < args.size(); ++i) {
        if (std::string_view{args[i]} == name) {
            return static_cast<int>(number(args[i + 1]).value_or(0xFFFF));
        }
    }
    return -1;
}

int fake_worker(std::span<char*> args) {
    if (ipc::block_stop_signals()) {
        return 1;
    }
    const int supervisor_fd = fd_option(args, "--supervisor-fd");
    if (!ipc::adopt_socket(supervisor_fd)) {
        return 1;
    }
    // Its log segment, kept open and unused: the supervisor reads an empty
    // ring.
    const int log_fd = fd_option(args, "--log-fd");
    const ipc::Fd log{log_fd >= 0 && ipc::adopt_descriptor(log_fd) ? log_fd : -1};
    FakeWorker worker{ipc::Connection{ipc::Fd{supervisor_fd}}};
    return worker.run();
}

} // namespace

int main(int argc, char** argv) {
    try {
        return fake_worker(std::span<char*>{argv, static_cast<std::size_t>(argc)});
    } catch (...) {
        return 2;
    }
}
