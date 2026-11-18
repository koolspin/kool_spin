// SPDX-License-Identifier: GPL-3.0-or-later
// SegmentTrack: mapping a track segment, and passing a worker's progress on to
// the audio thread (specs/track-loading/spec.md requirements 4 and 5), with
// the test writing the segment as a worker would.

#include <ks/engine_app/segment_track.h>

#include <ks/engine/track.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/layout.h>
#include <ks/ipc/region.h>
#include <ks/ipc/segment.h>
#include <ks/ipc/state.h>
#include <ks/ipc/track_segment.h>

#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace {

using ks::engine_app::SegmentTrack;
using ks::engine_app::TrackPass;
using ks::ipc::kMaxTrackFrames;
using ks::ipc::kTrackFrameSize;
using ks::ipc::kTrackLayoutVersion;
using ks::ipc::kTrackSamplesOffset;
using ks::ipc::kTrackSegmentSize;
using ks::ipc::SegmentKind;
using ks::ipc::TrackStatus;

constexpr std::uint32_t kRate = 44'100;

/// A private temporary directory, removed when destroyed.
class TempDir {
public:
    TempDir() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "ks-segment-track-XXXXXX").string();
        if (::mkdtemp(pattern.data()) != nullptr) {
            path_ = pattern;
        }
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

/// Creates segments, and writes track segments as a worker would.
class Worker {
public:
    Worker() {
        REQUIRE(!dir_.path().empty());
        std::expected<ks::ipc::SegmentFactory, std::error_code> factory =
            ks::ipc::SegmentFactory::open(dir_.path());
        REQUIRE(factory.has_value());
        factory_ = std::make_unique<ks::ipc::SegmentFactory>(std::move(*factory));
    }

    /// A new segment.
    ks::ipc::Fd create(SegmentKind kind = SegmentKind::Track,
                       std::uint32_t version = kTrackLayoutVersion,
                       std::size_t size = kTrackSegmentSize) {
        std::expected<ks::ipc::Fd, std::error_code> fd = factory_->create(kind, version, size);
        REQUIRE(fd.has_value());
        return std::move(*fd);
    }

    /// A new track segment, with the worker's mapping and writer, at `rate`.
    struct Track {
        ks::ipc::Fd fd;
        std::unique_ptr<ks::ipc::Region> region;
        std::unique_ptr<ks::ipc::TrackWriter> writer;

        [[nodiscard]] ks::ipc::TrackHeader& header() const {
            auto* const header = region->at<ks::ipc::TrackHeader>(ks::ipc::kSegmentHeaderSize);
            REQUIRE(header != nullptr);
            return *header;
        }
    };
    Track track(std::uint32_t rate = kRate) {
        Track track{.fd = create(), .region = nullptr, .writer = nullptr};
        std::expected<ks::ipc::Region, ks::ipc::MapFailure> region = ks::ipc::Region::map(
            track.fd.get(), SegmentKind::Track, kTrackLayoutVersion, ks::ipc::Access::ReadWrite);
        REQUIRE(region.has_value());
        track.region = std::make_unique<ks::ipc::Region>(std::move(*region));
        track.writer = std::make_unique<ks::ipc::TrackWriter>(track.region->bytes());
        track.writer->start(rate);
        return track;
    }

private:
    TempDir dir_;
    std::unique_ptr<ks::ipc::SegmentFactory> factory_;
};

/// The engine's SegmentTrack on a duplicate of `fd`, which must map.
std::unique_ptr<SegmentTrack> engine_track(const ks::ipc::Fd& fd, std::uint64_t id = 1,
                                           std::uint32_t rate = kRate) {
    std::expected<std::unique_ptr<SegmentTrack>, std::string> track =
        SegmentTrack::map(ks::ipc::Fd{::dup(fd.get())}, id, rate);
    REQUIRE(track.has_value());
    return std::move(*track);
}

/// Why SegmentTrack::map() refused a duplicate of `fd`; empty if it mapped.
std::string refusal(const ks::ipc::Fd& fd) {
    const std::expected<std::unique_ptr<SegmentTrack>, std::string> track =
        SegmentTrack::map(ks::ipc::Fd{::dup(fd.get())}, 1, kRate);
    return track ? std::string{} : track.error();
}

/// A budget that faults in everything.
constexpr std::size_t kAll = kMaxTrackFrames * kTrackFrameSize;

} // namespace

TEST_CASE("Segment track: mapping", "[engine_app]") {
    Worker worker;

    SECTION("a track segment") {
        const ks::ipc::Fd fd = worker.create();
        const std::unique_ptr<SegmentTrack> track = engine_track(fd, 7, 48'000);
        CHECK(track->id() == 7);
        CHECK(track->track().id() == 7);
        CHECK(track->track().sample_rate() == 48'000);
        CHECK(track->track().capacity() == kMaxTrackFrames);
        CHECK(track->track().ready().frames == 0);
        CHECK_FALSE(track->track().ready().complete);
        CHECK_FALSE(track->finished());
    }
    SECTION("a smaller track segment holds what its mapping holds") {
        const ks::ipc::Fd fd =
            worker.create(SegmentKind::Track, kTrackLayoutVersion, kTrackSamplesOffset + 65'536);
        CHECK(engine_track(fd)->track().capacity() == 65'536 / kTrackFrameSize);
    }
    SECTION("what it refuses") {
        CHECK(refusal(worker.create(SegmentKind::EngineState, ks::ipc::kEngineStateLayoutVersion,
                                    ks::ipc::kEngineStateSegmentSize)) ==
              "the segment isn't a track segment");
        CHECK(refusal(worker.create(SegmentKind::Track, kTrackLayoutVersion + 1)) ==
              "the segment isn't a track segment");
        CHECK(refusal(worker.create(SegmentKind::Track, kTrackLayoutVersion,
                                    kTrackSamplesOffset)) == "the segment holds no frames");
    }
}

TEST_CASE("Segment track: passing progress on", "[engine_app]") {
    Worker worker;
    const Worker::Track written = worker.track();
    const std::unique_ptr<SegmentTrack> track = engine_track(written.fd);

    // Nothing published: nothing passed on.
    CHECK(track->pass(kAll).faulted == 0);
    CHECK(track->track().ready().frames == 0);

    written.writer->publish(1'000);
    const TrackPass first = track->pass(kAll);
    CHECK(first.faulted == 1'000 * kTrackFrameSize);
    CHECK_FALSE(first.finished.has_value());
    CHECK(track->passed_on() == 1'000);
    CHECK(track->track().ready().frames == 1'000);
    CHECK_FALSE(track->track().ready().complete);

    written.writer->finish(TrackStatus::Truncated, 3'000);
    const TrackPass last = track->pass(kAll);
    CHECK(last.faulted == 2'000 * kTrackFrameSize);
    REQUIRE(last.finished.has_value());
    CHECK(last.finished.value_or(ks::engine_app::TrackFinished{}).frames == 3'000);
    CHECK(last.finished.value_or(ks::engine_app::TrackFinished{}).status == TrackStatus::Truncated);
    CHECK_FALSE(last.finished.value_or(ks::engine_app::TrackFinished{}).corrupt.has_value());
    CHECK(track->finished());
    CHECK(track->track().ready().frames == 3'000);
    CHECK(track->track().ready().complete);

    // Finished: the header isn't read again.
    ks::ipc::store_shared(written.header().ready, 2, std::memory_order_release);
    CHECK_FALSE(track->pass(kAll).finished.has_value());
    CHECK(track->track().ready().frames == 3'000);
}

TEST_CASE("Segment track: the budget limits each pass", "[engine_app]") {
    Worker worker;
    const Worker::Track written = worker.track();
    const std::unique_ptr<SegmentTrack> track = engine_track(written.fd);
    written.writer->finish(TrackStatus::Complete, 100'000);

    // 10,000 frames' worth a pass: complete only once all are passed on.
    constexpr std::size_t kBudget = 10'000 * kTrackFrameSize;
    for (std::uint64_t pass = 1; pass < 10; ++pass) {
        CAPTURE(pass);
        const TrackPass result = track->pass(kBudget);
        CHECK(result.faulted == kBudget);
        CHECK_FALSE(result.finished.has_value());
        CHECK(track->track().ready().frames == pass * 10'000);
        CHECK_FALSE(track->track().ready().complete);
    }
    const TrackPass last = track->pass(kBudget);
    CHECK(last.finished.has_value());
    CHECK(track->track().ready().frames == 100'000);
    CHECK(track->track().ready().complete);

    // No budget: nothing passed on, and a complete header doesn't finish a
    // track whose frames aren't all passed on.
    const Worker::Track other = worker.track();
    const std::unique_ptr<SegmentTrack> starved = engine_track(other.fd, 2);
    other.writer->finish(TrackStatus::Complete, 5);
    CHECK(starved->pass(0).faulted == 0);
    CHECK_FALSE(starved->finished());
    CHECK(starved->track().ready().frames == 0);
    CHECK_FALSE(starved->track().ready().complete);
}

TEST_CASE("Segment track: a corrupt header finishes it", "[engine_app]") {
    Worker worker;
    const Worker::Track written = worker.track();
    const std::unique_ptr<SegmentTrack> track = engine_track(written.fd);
    written.writer->publish(2'000);
    REQUIRE(track->pass(kAll).faulted == 2'000 * kTrackFrameSize);

    SECTION("a status that isn't a TrackStatus") {
        ks::ipc::store_shared(written.header().status, 7, std::memory_order_relaxed);
    }
    SECTION("frames that go down") {
        ks::ipc::store_shared(written.header().ready, 1'999, std::memory_order_release);
    }
    SECTION("a rate that isn't the loaded rate") {
        ks::ipc::store_shared(written.header().sample_rate, 48'000, std::memory_order_relaxed);
    }
    SECTION("a rate out of range") {
        ks::ipc::store_shared(written.header().sample_rate, 0, std::memory_order_relaxed);
    }

    const TrackPass pass = track->pass(kAll);
    REQUIRE(pass.finished.has_value());
    const ks::engine_app::TrackFinished finished =
        pass.finished.value_or(ks::engine_app::TrackFinished{});
    CHECK(finished.frames == 2'000);
    CHECK(finished.corrupt.has_value());
    CHECK(track->finished());
    CHECK(track->track().ready().frames == 2'000);
    CHECK(track->track().ready().complete);

    // More frames afterwards change nothing.
    written.writer->publish(10'000);
    CHECK_FALSE(track->pass(kAll).finished.has_value());
    CHECK(track->track().ready().frames == 2'000);
}

TEST_CASE("Segment track: a rate that differs is corrupt only once frames come", "[engine_app]") {
    Worker worker;
    const Worker::Track written = worker.track(48'000);
    const std::unique_ptr<SegmentTrack> track = engine_track(written.fd, 1, kRate);
    // The worker has stored its rate, but published nothing.
    CHECK_FALSE(track->pass(kAll).finished.has_value());
    written.writer->publish(10);
    const TrackPass pass = track->pass(kAll);
    REQUIRE(pass.finished.has_value());
    CHECK(pass.finished.value_or(ks::engine_app::TrackFinished{}).frames == 0);
    CHECK(track->track().ready().complete);
}
