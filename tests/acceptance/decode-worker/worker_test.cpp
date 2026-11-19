// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/decode-worker/spec.md: ks-worker as a process,
// with the test as its supervisor (requirements 15 to 20).

#include <ks/decode/decoder.h>
#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/fd_passing.h>
#include <ks/ipc/frame.h>
#include <ks/ipc/log.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/region.h>
#include <ks/ipc/track_segment.h>
#include <ks/supervisor/spawn.h>

#include "decode_support.h"
#include "processes_support.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

using ks::decode::Decoder;
using ks::decode::Refusal;
using ks::decode::test::capacity_of;
using ks::decode::test::decode_all;
using ks::decode::test::fixture;
using ks::decode::test::frames_in;
using ks::decode::test::kLongFrames;
using ks::decode::test::kSecond;
using ks::decode::test::long_frame_right;
using ks::decode::test::make_segment;
using ks::decode::test::make_track_segment;
using ks::decode::test::open_decoder;
using ks::decode::test::open_file;
using ks::decode::test::progress_of;
using ks::decode::test::required_progress;
using ks::decode::test::samples_of;
using ks::decode::test::Segment;
using ks::decode::test::source_for;
using ks::decode::test::write_long_wav;
using ks::ipc::Connection;
using ks::ipc::ErrorCode;
using ks::ipc::Fd;
using ks::ipc::Frame;
using ks::ipc::kMaxTrackFrames;
using ks::ipc::kTrackFrameSize;
using ks::ipc::kTrackSamplesOffset;
using ks::ipc::LogLevel;
using ks::ipc::LogReader;
using ks::ipc::LogRecord;
using ks::ipc::LogSegment;
using ks::ipc::Message;
using ks::ipc::SegmentKind;
using ks::ipc::socket_pair;
using ks::ipc::TrackProgress;
using ks::ipc::TrackStatus;
using ks::ipc::test::set_nonblocking;
using ks::ipc::test::TempDir;
using ks::processes::test::error_code;
using ks::processes::test::eventually;
using ks::processes::test::is;
using ks::processes::test::Process;
using ks::processes::test::receive;
using ks::processes::test::Received;
using ks::processes::test::send;
using ks::supervisor::Inherit;
namespace msg = ks::ipc::msg;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

/// The program under test.
constexpr std::string_view kWorkerPath = KS_WORKER_PATH;

/// True when the test is built with AddressSanitizer or ThreadSanitizer,
/// which slow a process down: on macOS tsan, a stopping worker takes 130 to
/// 160 ms to exit.
constexpr bool kSlowSanitizer =
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

/// How long a stopped worker may take to exit: 100 ms (requirement 19), or
/// 300 ms under AddressSanitizer or ThreadSanitizer.
constexpr milliseconds kStopTime{kSlowSanitizer ? 300 : 100};

/// How many workers a stopping test stops, at most, for one to exit within
/// kStopTime.
constexpr std::size_t kStopTries = 5;

/// The longest a test waits for the long WAV to decode.
constexpr milliseconds kLongWait{60'000};

/// A worker started as the supervisor would start it, through a shell that
/// sends its standard error to a file: spawn() can't redirect it.
struct Worker {
    TempDir dir;
    std::filesystem::path stderr_path;
    /// The supervisor's end of the socket pair, non-blocking.
    std::unique_ptr<Connection> socket;
    /// The log segment, mapped read-write, if it has one.
    std::unique_ptr<Segment> log;
    Process process;
};

/// The shell command that runs "$@" with standard error appended to "$0".
constexpr std::string_view kWithStderr = R"(exec "$@" 2>>"$0")";

/// Starts `kWorkerPath` with `args`, inheriting `fds`, through the shell,
/// with standard error going to `stderr_path`.
Process start_with_stderr(const std::filesystem::path& stderr_path,
                          const std::vector<std::string>& args,
                          const std::vector<Inherit>& fds = {}) {
    std::vector<std::string> shell_args{"-c", std::string{kWithStderr}, stderr_path.string(),
                                        std::string{kWorkerPath}};
    shell_args.insert(shell_args.end(), args.begin(), args.end());
    return ks::processes::test::start("/bin/sh", shell_args, fds);
}

/// Starts a worker with its socket at 3 and, if `with_log`, a log segment at
/// 4.
std::unique_ptr<Worker> start_worker(bool with_log = false) {
    auto worker = std::make_unique<Worker>();
    REQUIRE(!worker->dir.path().empty());
    worker->stderr_path = worker->dir.path() / "stderr.txt";
    std::expected<std::pair<Fd, Fd>, std::error_code> pair = socket_pair();
    REQUIRE(pair.has_value());
    std::vector<Inherit> fds{{.parent = pair->second.get(), .child = 3}};
    std::vector<std::string> args{"--supervisor-fd", "3"};
    if (with_log) {
        worker->log =
            make_segment(SegmentKind::Log, ks::ipc::kLogLayoutVersion, ks::ipc::kLogSegmentSize);
        fds.push_back({.parent = worker->log->fd.get(), .child = 4});
        args.insert(args.end(), {"--log-fd", "4"});
    }
    worker->process = start_with_stderr(worker->stderr_path, args, fds);
    REQUIRE(worker->process.pid() > 0);
    // Only the worker holds its end now, so the test sees it close.
    pair->second.reset();
    REQUIRE(set_nonblocking(pair->first.get()));
    worker->socket = std::make_unique<Connection>(std::move(pair->first));
    return worker;
}

/// What the worker wrote to standard error.
std::string stderr_of(const Worker& worker) {
    std::ifstream file{worker.stderr_path};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

/// Sends a Probe for the file at `path`, with request ID `id`, and returns
/// the reply.
std::optional<Received> probe(Worker& worker, const std::filesystem::path& path, std::uint32_t id) {
    const Fd file = open_file(path);
    REQUIRE(file.get() >= 0);
    const std::vector<int> fds{file.get()};
    REQUIRE(send(*worker.socket, Message{.request_id = id, .body = msg::Probe{}}, fds));
    return receive(*worker.socket);
}

/// Sends a Decode of at most `max_frames` into `segment`, with request ID
/// `id`. The reply isn't read.
void send_decode(Worker& worker, const Segment& segment, std::uint64_t max_frames,
                 std::uint32_t id) {
    const std::vector<int> fds{segment.fd.get()};
    REQUIRE(send(*worker.socket,
                 Message{.request_id = id, .body = msg::Decode{.max_frames = max_frames}}, fds));
}

/// The Probed in `received`, or nothing if it isn't one.
std::optional<msg::Probed> probed_of(const std::optional<Received>& received) {
    if (!received) {
        return std::nullopt;
    }
    const auto* const probed = std::get_if<msg::Probed>(&received->message.body);
    if (probed == nullptr) {
        return std::nullopt;
    }
    return *probed;
}

/// The Decoded in `received`, or nothing if it isn't one.
std::optional<msg::Decoded> decoded_of(const std::optional<Received>& received) {
    if (!received) {
        return std::nullopt;
    }
    const auto* const decoded = std::get_if<msg::Decoded>(&received->message.body);
    if (decoded == nullptr) {
        return std::nullopt;
    }
    return *decoded;
}

/// The Error's text in `received`, or nothing if it isn't an Error.
std::optional<std::string> error_text(const std::optional<Received>& received) {
    if (!received) {
        return std::nullopt;
    }
    const auto* const error = std::get_if<msg::Error>(&received->message.body);
    if (error == nullptr) {
        return std::nullopt;
    }
    return error->text;
}

/// True if a Ping with request ID `id` is answered with a Pong with that ID.
bool pings(Worker& worker, std::uint32_t id) {
    return send(*worker.socket, Message{.request_id = id, .body = msg::Ping{}}) &&
           is<msg::Pong>(receive(*worker.socket), id);
}

/// Everything the file at `path` decodes to, in this process.
std::vector<float> reference(const std::filesystem::path& path) {
    Decoder decoder = open_decoder(path);
    return decode_all(decoder);
}

/// The lines in a log segment.
std::vector<LogRecord> records_of(const Segment& log) {
    auto* const segment = log.region->at<LogSegment>(ks::ipc::kSegmentHeaderSize);
    REQUIRE(segment != nullptr);
    LogReader reader{*segment};
    std::vector<LogRecord> records;
    while (const std::optional<LogRecord> record = reader.next()) {
        records.push_back(*record);
    }
    return records;
}

/// A record's text, with its length clamped to the record.
std::string text_of(const LogRecord& record) {
    const std::size_t length = std::min<std::uint64_t>(record.length, ks::ipc::kLogTextSize);
    const std::span<const std::uint8_t> bytes = std::span{record.text}.first(length);
    return std::string{bytes.begin(), bytes.end()};
}

/// True if a record in `records` is at `level` and contains `phrase`.
bool has_line(const std::vector<LogRecord>& records, LogLevel level, std::string_view phrase) {
    return std::ranges::any_of(records, [&](const LogRecord& record) {
        return record.level == static_cast<std::uint64_t>(level) &&
               text_of(record).find(phrase) != std::string::npos;
    });
}

/// The lines in `records`, for CAPTURE.
std::vector<std::string> texts_of(const std::vector<LogRecord>& records) {
    std::vector<std::string> texts;
    texts.reserve(records.size());
    for (const LogRecord& record : records) {
        texts.push_back(text_of(record));
    }
    return texts;
}

/// A worker paused with SIGSTOP part of the way through decoding the long
/// WAV: frames are ready, and the track isn't complete.
struct Paused {
    std::unique_ptr<Worker> worker;
    std::unique_ptr<Segment> segment;
};

/// Starts a worker on the long WAV at `path`, and pauses it once frames are
/// ready. If decoding finished first, starts again, up to 5 times, and fails
/// the test if every try finished first.
Paused pause_while_decoding(const std::filesystem::path& path) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        Paused paused{.worker = start_worker(), .segment = make_track_segment()};
        REQUIRE(probed_of(probe(*paused.worker, path, 1)).has_value());
        send_decode(*paused.worker, *paused.segment, kMaxTrackFrames, 2);
        REQUIRE(eventually([&] {
            const std::optional<TrackProgress> progress = progress_of(*paused.segment);
            return progress && progress->frames > 0;
        }));
        const pid_t pid = paused.worker->process.pid();
        REQUIRE(::kill(pid, SIGSTOP) == 0);
        // Wait until it has stopped, or exited, without reaping it: Process
        // does that.
        siginfo_t info{};
        int result = ::waitid(P_PID, static_cast<id_t>(pid), &info, WSTOPPED | WEXITED | WNOWAIT);
        while (result == -1 && errno == EINTR) {
            result = ::waitid(P_PID, static_cast<id_t>(pid), &info, WSTOPPED | WEXITED | WNOWAIT);
        }
        REQUIRE(result == 0);
        if (info.si_code != CLD_STOPPED) {
            // It exited: decoding finished first.
            continue;
        }
        if (!required_progress(*paused.segment).complete) {
            return paused;
        }
        ::kill(pid, SIGCONT);
    }
    FAIL("decoding finished before the worker could be paused, 5 times");
    return {};
}

/// How long `process` takes to exit after `since`. It must exit within ten
/// times kStopTime.
milliseconds exit_time(Process& process, Clock::time_point since) {
    REQUIRE(process.wait(kStopTime * 10).has_value());
    return std::chrono::duration_cast<milliseconds>(Clock::now() - since);
}

/// Calls `stop`, which starts a worker, stops it, checks how it ended, and
/// returns how long it took to exit, until a worker exits within kStopTime
/// or kStopTries have been stopped. Returns each worker's exit time. A busy
/// machine can hold up a worker that stopped in time, but not every one.
template <std::invocable Stop>
std::vector<milliseconds::rep> stop_times(Stop stop) {
    std::vector<milliseconds::rep> times;
    while (times.size() < kStopTries && (times.empty() || milliseconds{times.back()} > kStopTime)) {
        const milliseconds took = stop();
        times.push_back(took.count());
    }
    return times;
}

} // namespace

TEST_CASE("Worker: probing and decoding a file", "[worker]") {
    const std::vector<float> expected = reference(fixture("tone.flac"));
    REQUIRE(frames_in(expected) == kSecond);

    const std::unique_ptr<Worker> worker = start_worker();
    const std::optional<Received> probed = probe(*worker, fixture("tone.flac"), 1);
    REQUIRE(is<msg::Probed>(probed, 1));
    CHECK(probed_of(probed) ==
          msg::Probed{.sample_rate = 44'100, .channels = 2, .estimated_frames = kSecond});

    const std::unique_ptr<Segment> segment = make_track_segment();
    send_decode(*worker, *segment, kMaxTrackFrames, 2);
    const std::optional<Received> decoded = receive(*worker->socket);
    REQUIRE(is<msg::Decoded>(decoded, 2));
    CHECK(decoded_of(decoded) == msg::Decoded{.frames = kSecond, .status = TrackStatus::Complete});

    CHECK(progress_of(*segment) == TrackProgress{.sample_rate = 44'100,
                                                 .status = TrackStatus::Complete,
                                                 .frames = kSecond,
                                                 .complete = true});
    const std::span<const float> samples = samples_of(*segment);
    CHECK(std::equal(expected.begin(), expected.end(), samples.begin()));
    CHECK(worker->process.wait() == 0);
}

TEST_CASE("Worker: frames are published as they're decoded", "[worker]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());
    const std::filesystem::path path = dir.path() / "long.wav";
    write_long_wav(path);

    const std::unique_ptr<Worker> worker = start_worker();
    const std::optional<Received> probed = probe(*worker, path, 1);
    REQUIRE(is<msg::Probed>(probed, 1));
    CHECK(probed_of(probed) ==
          msg::Probed{.sample_rate = 44'100, .channels = 2, .estimated_frames = kLongFrames});

    const std::unique_ptr<Segment> segment = make_track_segment();
    const std::span<const float> samples = samples_of(*segment);
    send_decode(*worker, *segment, kMaxTrackFrames, 2);

    // Read `ready` as fast as possible until the track is complete.
    std::set<std::uint64_t> seen;
    std::uint64_t last = 0;
    bool went_down = false;
    bool frame_wrong = false;
    const Clock::time_point deadline = Clock::now() + kLongWait;
    TrackProgress progress;
    while (Clock::now() < deadline) {
        progress = required_progress(*segment);
        if (progress.complete) {
            break;
        }
        if (progress.frames < last) {
            went_down = true;
        }
        if (progress.frames > 0 && progress.frames != last) {
            seen.insert(progress.frames);
            // The last frame published already holds its final values.
            if (!long_frame_right(samples, progress.frames - 1)) {
                frame_wrong = true;
            }
        }
        last = progress.frames;
    }
    REQUIRE(progress.complete);
    CAPTURE(seen.size());
    CHECK(seen.size() >= 10);
    CHECK_FALSE(went_down);
    CHECK_FALSE(frame_wrong);

    const std::optional<Received> decoded = receive(*worker->socket);
    REQUIRE(is<msg::Decoded>(decoded, 2));
    CHECK(decoded_of(decoded) ==
          msg::Decoded{.frames = kLongFrames, .status = TrackStatus::Complete});
    CHECK(progress.frames == kLongFrames);
    CHECK(progress.status == TrackStatus::Complete);
    bool all_right = true;
    for (std::uint64_t frame = 0; frame < kLongFrames && all_right; ++frame) {
        all_right = long_frame_right(samples, frame);
    }
    CHECK(all_right);
    CHECK(worker->process.wait() == 0);
}

TEST_CASE("Worker: refusing a file and a segment", "[worker]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());

    SECTION("a file it can't decode") {
        const std::filesystem::path notes = dir.path() / "notes.txt";
        std::ofstream{notes} << "These are not the samples you're looking for.\n";
        const std::expected<Decoder, Refusal> refused = Decoder::open(source_for(notes));
        REQUIRE_FALSE(refused.has_value());

        const std::unique_ptr<Worker> worker = start_worker();
        const std::optional<Received> reply = probe(*worker, notes, 1);
        REQUIRE(is<msg::Error>(reply, 1));
        CHECK(error_code(reply) == ErrorCode::Refused);
        CHECK(error_text(reply) == refused.error().text);
        CHECK(worker->process.wait() == 0);
    }

    SECTION("a segment of the wrong kind") {
        const std::unique_ptr<Worker> worker = start_worker();
        REQUIRE(is<msg::Probed>(probe(*worker, fixture("tone.flac"), 1), 1));
        const std::unique_ptr<Segment> log =
            make_segment(SegmentKind::Log, ks::ipc::kLogLayoutVersion, ks::ipc::kLogSegmentSize);
        send_decode(*worker, *log, kMaxTrackFrames, 2);
        const std::optional<Received> reply = receive(*worker->socket);
        REQUIRE(is<msg::Error>(reply, 2));
        CHECK(error_code(reply) == ErrorCode::Refused);
        CHECK(worker->process.wait() == 0);
    }

    SECTION("a segment with too little room") {
        const std::unique_ptr<Worker> worker = start_worker();
        REQUIRE(is<msg::Probed>(probe(*worker, fixture("tone.flac"), 1), 1));
        const std::unique_ptr<Segment> small =
            make_track_segment(kTrackSamplesOffset + (1'000 * kTrackFrameSize));
        REQUIRE(capacity_of(*small) < kSecond);
        send_decode(*worker, *small, kSecond, 2);
        const std::optional<Received> reply = receive(*worker->socket);
        REQUIRE(is<msg::Error>(reply, 2));
        CHECK(error_code(reply) == ErrorCode::Refused);
        CHECK(worker->process.wait() == 0);
        // Nothing was written.
        CHECK(progress_of(*small) == TrackProgress{});
    }
}

TEST_CASE("Worker: messages out of turn", "[worker]") {
    SECTION("the job carries on") {
        const std::unique_ptr<Worker> worker = start_worker();
        CHECK(pings(*worker, 1));

        // A Decode before a Probe.
        const std::unique_ptr<Segment> early = make_track_segment();
        send_decode(*worker, *early, kMaxTrackFrames, 2);
        const std::optional<Received> refused = receive(*worker->socket);
        CHECK(is<msg::Error>(refused, 2));
        CHECK(error_code(refused) == ErrorCode::NotSupported);
        CHECK(progress_of(*early) == TrackProgress{});

        REQUIRE(is<msg::Probed>(probe(*worker, fixture("tone.flac"), 3), 3));
        CHECK(pings(*worker, 4));

        // A second Probe.
        const std::optional<Received> again = probe(*worker, fixture("tone.flac"), 5);
        CHECK(is<msg::Error>(again, 5));
        CHECK(error_code(again) == ErrorCode::NotSupported);

        const std::unique_ptr<Segment> segment = make_track_segment();
        send_decode(*worker, *segment, kMaxTrackFrames, 6);
        const std::optional<Received> decoded = receive(*worker->socket);
        REQUIRE(is<msg::Decoded>(decoded, 6));
        CHECK(decoded_of(decoded) ==
              msg::Decoded{.frames = kSecond, .status = TrackStatus::Complete});
        CHECK(worker->process.wait() == 0);
    }

    SECTION("a frame that doesn't decode") {
        const std::unique_ptr<Worker> worker = start_worker();
        // Type 99 isn't a message type.
        const Frame unknown{.type = 99, .request_id = 7, .descriptor_count = 0, .body = {}};
        REQUIRE(worker->socket->send(unknown, {}));
        REQUIRE(worker->socket->flush());
        const std::optional<Received> reply = receive(*worker->socket);
        CHECK(is<msg::Error>(reply, 7));
        CHECK(error_code(reply) == ErrorCode::BadMessage);
        CHECK(worker->process.wait() == 0);
    }
}

TEST_CASE("Worker: stopping", "[worker]") {
    SECTION("SIGTERM while it waits for a Probe") {
        const std::vector<milliseconds::rep> times = stop_times([] {
            const std::unique_ptr<Worker> worker = start_worker();
            // Once it answers, it's running, with its signals blocked.
            REQUIRE(pings(*worker, 1));
            const Clock::time_point sent = Clock::now();
            worker->process.signal(SIGTERM);
            const milliseconds took = exit_time(worker->process, sent);
            CHECK(worker->process.wait() == 0);
            return took;
        });
        CAPTURE(times);
        CHECK(milliseconds{times.back()} <= kStopTime);
    }

    SECTION("the socket closing after Probed") {
        const std::unique_ptr<Worker> worker = start_worker();
        REQUIRE(is<msg::Probed>(probe(*worker, fixture("tone.flac"), 1), 1));
        worker->socket.reset();
        CHECK(worker->process.wait() == 0);
    }

    SECTION("while it decodes") {
        const TempDir dir;
        REQUIRE(!dir.path().empty());
        const std::filesystem::path path = dir.path() / "long.wav";
        write_long_wav(path);

        SECTION("SIGTERM") {
            const std::vector<milliseconds::rep> times = stop_times([&] {
                const Paused paused = pause_while_decoding(path);
                Worker& worker = *paused.worker;
                worker.process.signal(SIGTERM);
                const Clock::time_point resumed = Clock::now();
                REQUIRE(::kill(worker.process.pid(), SIGCONT) == 0);
                const milliseconds took = exit_time(worker.process, resumed);
                CHECK(worker.process.wait() == 0);

                const TrackProgress progress = required_progress(*paused.segment);
                CHECK(progress.complete);
                CHECK(progress.status == TrackStatus::Failed);
                CHECK(progress.frames < kLongFrames);
                const std::span<const float> samples = samples_of(*paused.segment);
                bool all_right = true;
                for (std::uint64_t frame = 0; frame < progress.frames && all_right; ++frame) {
                    all_right = long_frame_right(samples, frame);
                }
                CHECK(all_right);
                // It didn't reply.
                CHECK_FALSE(receive(*worker.socket).has_value());
                return took;
            });
            CAPTURE(times);
            CHECK(milliseconds{times.back()} <= kStopTime);
        }

        SECTION("the socket closing") {
            const Paused paused = pause_while_decoding(path);
            Worker& worker = *paused.worker;
            worker.socket.reset();
            REQUIRE(::kill(worker.process.pid(), SIGCONT) == 0);
            CHECK(worker.process.wait(kLongWait) == 0);

            CHECK(required_progress(*paused.segment) ==
                  TrackProgress{.sample_rate = 44'100,
                                .status = TrackStatus::Complete,
                                .frames = kLongFrames,
                                .complete = true});
            const std::span<const float> samples = samples_of(*paused.segment);
            bool all_right = true;
            for (std::uint64_t frame = 0; frame < kLongFrames && all_right; ++frame) {
                all_right = long_frame_right(samples, frame);
            }
            CHECK(all_right);
        }
    }
}

TEST_CASE("Worker: its log", "[worker]") {
    SECTION("a decoded track") {
        const std::unique_ptr<Worker> worker = start_worker(true);
        REQUIRE(is<msg::Probed>(probe(*worker, fixture("tone.flac"), 1), 1));
        const std::unique_ptr<Segment> segment = make_track_segment();
        send_decode(*worker, *segment, kMaxTrackFrames, 2);
        REQUIRE(is<msg::Decoded>(receive(*worker->socket), 2));
        REQUIRE(worker->process.wait() == 0);
        const std::vector<LogRecord> records = records_of(*worker->log);
        CAPTURE(texts_of(records));
        CHECK(has_line(records, LogLevel::Info, "decoded 44100"));
        CHECK(stderr_of(*worker).empty());
    }

    SECTION("a refused file") {
        const TempDir dir;
        REQUIRE(!dir.path().empty());
        const std::filesystem::path notes = dir.path() / "notes.txt";
        std::ofstream{notes} << "These are not the samples you're looking for.\n";
        const std::unique_ptr<Worker> worker = start_worker(true);
        REQUIRE(is<msg::Error>(probe(*worker, notes, 1), 1));
        REQUIRE(worker->process.wait() == 0);
        const std::vector<LogRecord> records = records_of(*worker->log);
        CAPTURE(texts_of(records));
        CHECK(has_line(records, LogLevel::Warning, "refused: "));
        CHECK(stderr_of(*worker).empty());
    }

    SECTION("a truncated track") {
        const std::unique_ptr<Worker> worker = start_worker(true);
        REQUIRE(is<msg::Probed>(probe(*worker, fixture("tone.flac"), 1), 1));
        const std::unique_ptr<Segment> segment = make_track_segment();
        send_decode(*worker, *segment, 10'000, 2);
        const std::optional<Received> decoded = receive(*worker->socket);
        REQUIRE(is<msg::Decoded>(decoded, 2));
        CHECK(decoded_of(decoded) ==
              msg::Decoded{.frames = 10'000, .status = TrackStatus::Truncated});
        REQUIRE(worker->process.wait() == 0);
        const std::vector<LogRecord> records = records_of(*worker->log);
        CAPTURE(texts_of(records));
        CHECK(has_line(records, LogLevel::Warning, "truncated at 10000"));
        CHECK(stderr_of(*worker).empty());
    }
}

TEST_CASE("Worker: usage", "[worker]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());

    struct Case {
        const char* name;
        std::vector<std::string> args;
    };
    for (const Case& c : std::vector<Case>{
             {"an unknown option", {"--supervisor-fd", "3", "--bogus"}},
             {"no --supervisor-fd", {}},
             {"a repeated option", {"--supervisor-fd", "3", "--supervisor-fd", "3"}},
         }) {
        CAPTURE(c.name);
        const std::filesystem::path stderr_path = dir.path() / (std::string{c.name} + ".txt");
        Process process = start_with_stderr(stderr_path, c.args);
        REQUIRE(process.pid() > 0);
        CHECK(process.wait() == 2);
        std::ifstream file{stderr_path};
        std::string first;
        std::getline(file, first);
        CAPTURE(first);
        CHECK(first.starts_with("ks-worker: "));
    }

    const std::filesystem::path help_stderr = dir.path() / "help.txt";
    Process help = start_with_stderr(help_stderr, {"--help"});
    REQUIRE(help.pid() > 0);
    CHECK(help.wait() == 0);
}
