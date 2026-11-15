// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/segments/spec.md: creating segments, passing
// them between processes, and mapping them.

#include <ks/ipc/segment.h>

#include <ks/ipc/fd.h>
#include <ks/ipc/fd_passing.h>
#include <ks/ipc/region.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/spsc_ring.h>

#include "segments_support.h"
#include "test_support.h"
#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using ks::ipc::Access;
using ks::ipc::check_header;
using ks::ipc::Fd;
using ks::ipc::kSegmentHeaderSize;
using ks::ipc::MapError;
using ks::ipc::MapFailure;
using ks::ipc::receive_some;
using ks::ipc::Region;
using ks::ipc::RingConsumer;
using ks::ipc::RingProducer;
using ks::ipc::SegmentFactory;
using ks::ipc::SegmentHeader;
using ks::ipc::SegmentKind;
using ks::ipc::send_some;
using ks::ipc::Seqlock;
using ks::ipc::SeqlockReader;
using ks::ipc::SeqlockWriter;
using ks::ipc::socket_pair;
using ks::ipc::SpscRing;
using ks::ipc::test::Child;
using ks::ipc::test::is_cloexec;
using ks::ipc::test::is_consistent;
using ks::ipc::test::is_item;
using ks::ipc::test::Item;
using ks::ipc::test::make_item;
using ks::ipc::test::make_value;
using ks::ipc::test::page_size;
using ks::ipc::test::StallWatch;
using ks::ipc::test::TempDir;
using ks::ipc::test::Value;

constexpr std::uint32_t kVersion = 7;

/// A ring and a seqlock, as a client's command ring and the engine's state
/// would hold them.
struct Layout {
    SpscRing<Item, 8> ring;
    Seqlock<Value> newest;
};

/// The error Region::map() reports, or nothing if it maps the segment.
std::optional<MapError> refusal(int fd, SegmentKind kind, std::uint32_t layout_version) {
    const std::expected<Region, MapFailure> region =
        Region::map(fd, kind, layout_version, Access::ReadOnly);
    if (region) {
        return std::nullopt;
    }
    return region.error().error;
}

/// Sends one byte and `fd` over `socket`. True if it went.
bool send_fd(int socket, int fd) {
    const std::array bytes{std::byte{1}};
    const std::array fds{fd};
    const std::expected<std::size_t, std::error_code> sent = send_some(socket, bytes, fds);
    return sent.has_value() && *sent == 1;
}

/// Receives one byte and one descriptor from `socket`, or nothing.
Fd receive_fd(int socket) {
    std::array<std::byte, 1> byte{};
    std::vector<Fd> fds;
    const std::expected<std::size_t, std::error_code> received = receive_some(socket, byte, fds);
    if (!received || *received != 1 || fds.size() != 1) {
        return Fd{};
    }
    return std::move(fds.front());
}

/// The forked child's side of "shared across processes": receives the
/// segment, then pushes every item and publishes the newest in the seqlock.
/// Returns the child's exit code.
int produce(int socket, std::uint64_t items) {
    const Fd fd = receive_fd(socket);
    if (!fd.valid()) {
        return 2;
    }
    std::expected<Region, MapFailure> region =
        Region::map(fd.get(), SegmentKind::CommandRing, kVersion, Access::ReadWrite);
    if (!region) {
        return 3;
    }
    auto* const layout = region->at<Layout>(kSegmentHeaderSize);
    if (layout == nullptr) {
        return 4;
    }
    RingProducer producer{layout->ring};
    SeqlockWriter writer{layout->newest};
    StallWatch watch;
    for (std::uint64_t n = 0; n < items;) {
        if (producer.try_push(make_item(n))) {
            writer.write(make_value(n));
            ++n;
            watch.progressed();
        } else if (watch.stalled()) {
            return 1;
        } else {
            std::this_thread::yield();
        }
    }
    return 0;
}

/// A process's peak resident memory, in getrusage()'s units.
std::int64_t max_rss(const rusage& usage) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): glibc puts ru_maxrss in a union
    return static_cast<std::int64_t>(usage.ru_maxrss);
}

#ifdef __linux__
/// How many of this process's mappings are kool_spin memfds.
int memfd_mappings() {
    std::ifstream maps{"/proc/self/maps"};
    int count = 0;
    for (std::string line; std::getline(maps, line);) {
        if (line.find("/memfd:kool_spin") != std::string::npos) {
            ++count;
        }
    }
    return count;
}

/// A memfd with sealing allowed but no seals yet, `size` bytes long.
Fd unsealed_memfd(std::size_t size) {
    Fd fd{::memfd_create("ks-test", MFD_CLOEXEC | MFD_ALLOW_SEALING)};
    if (fd.valid() && ::ftruncate(fd.get(), static_cast<off_t>(size)) == -1) {
        fd.reset();
    }
    return fd;
}

/// Adds `seals` to `fd`. True if it worked.
bool add_seals(int fd, int seals) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX declares fcntl() variadic
    return ::fcntl(fd, F_ADD_SEALS, seals) != -1;
}
#endif

#ifdef __APPLE__
/// `length` random letters and digits.
std::string random_letters(std::size_t length) {
    constexpr std::string_view kAlphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device device;
    std::uniform_int_distribution<std::size_t> pick{0, kAlphabet.size() - 1};
    std::string text;
    for (std::size_t i = 0; i < length; ++i) {
        text += kAlphabet[pick(device)];
    }
    return text;
}

/// shm_open(), whose mode argument POSIX declares variadic.
int open_shared_memory(const std::string& name, int flags) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX declares shm_open() variadic
    return ::shm_open(name.c_str(), flags, 0600);
}

/// Creates a shared-memory object called `name`, and leaves it behind.
bool leave_object(const std::string& name) {
    const Fd fd{open_shared_memory(name, O_RDWR | O_CREAT | O_EXCL)};
    return fd.valid();
}

/// True if a shared-memory object called `name` exists.
bool object_exists(const std::string& name) {
    const Fd fd{open_shared_memory(name, O_RDONLY)};
    return fd.valid();
}

/// Replaces the contents of `path` with `text`.
void write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << text;
}
#endif

} // namespace

TEST_CASE("Segments: created zero-filled with a header", "[segments]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());
    std::expected<SegmentFactory, std::error_code> factory = SegmentFactory::open(dir.path());
    REQUIRE(factory.has_value());
    const std::size_t page = page_size();

    for (const std::size_t size :
         {std::size_t{1}, kSegmentHeaderSize, page, page + 1, (3 * page) + 17}) {
        CAPTURE(size);
        const std::size_t expected = (size + page - 1) / page * page;
        const std::expected<Fd, std::error_code> fd =
            factory->create(SegmentKind::Track, kVersion, size);
        REQUIRE(fd.has_value());
        REQUIRE(is_cloexec(fd->get()));

        struct stat status{};
        REQUIRE(::fstat(fd->get(), &status) == 0);
        REQUIRE(static_cast<std::size_t>(status.st_size) == expected);

        const std::expected<Region, MapFailure> region =
            Region::map(fd->get(), SegmentKind::Track, kVersion, Access::ReadWrite);
        REQUIRE(region.has_value());
        const std::span<std::byte> bytes = region->bytes();
        REQUIRE(bytes.size() == expected);
        REQUIRE(check_header(bytes, SegmentKind::Track, kVersion).has_value());
        REQUIRE(std::ranges::all_of(bytes.subspan(sizeof(SegmentHeader)),
                                    [](std::byte b) { return b == std::byte{0}; }));
    }

    const std::expected<Fd, std::error_code> empty =
        factory->create(SegmentKind::Track, kVersion, 0);
    REQUIRE_FALSE(empty.has_value());
    REQUIRE(empty.error() == std::errc::invalid_argument);

    const std::expected<Fd, std::error_code> huge =
        factory->create(SegmentKind::Track, kVersion, std::numeric_limits<std::size_t>::max());
    REQUIRE_FALSE(huge.has_value());
    REQUIRE(huge.error() == std::errc::value_too_large);
}

TEST_CASE("Segments: shared across processes by descriptor", "[segments]") {
    constexpr std::uint64_t kItems = 200'000;
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());
    std::expected<std::pair<Fd, Fd>, std::error_code> sockets = socket_pair();
    REQUIRE(sockets.has_value());
    REQUIRE(is_cloexec(sockets->first.get()));
    REQUIRE(is_cloexec(sockets->second.get()));

    // The child exists before the segment does, so it can only get it over
    // the socket.
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        sockets->first.reset();
        ::_exit(produce(sockets->second.get(), kItems));
    }
    Child child{pid};
    sockets->second.reset();

    std::expected<SegmentFactory, std::error_code> factory = SegmentFactory::open(dir.path());
    REQUIRE(factory.has_value());
    const std::expected<Fd, std::error_code> fd =
        factory->create(SegmentKind::CommandRing, kVersion, kSegmentHeaderSize + sizeof(Layout));
    REQUIRE(fd.has_value());
    const std::expected<Region, MapFailure> region =
        Region::map(fd->get(), SegmentKind::CommandRing, kVersion, Access::ReadWrite);
    REQUIRE(region.has_value());
    auto* const layout = region->at<Layout>(kSegmentHeaderSize);
    REQUIRE(layout != nullptr);
    REQUIRE(send_fd(sockets->first.get(), fd->get()));

    // The parent pops every item, and reads the seqlock as it goes.
    RingConsumer consumer{layout->ring};
    const SeqlockReader reader{layout->newest};
    StallWatch watch;
    std::uint64_t newest = 0;
    std::string error;
    for (std::uint64_t n = 0; n < kItems && error.empty();) {
        if (const std::optional<Item> item = consumer.try_pop()) {
            if (!is_item(*item, n)) {
                error = std::format("expected item {}, got item {} or a damaged copy", n,
                                    item->sequence);
            }
            ++n;
            watch.progressed();
        } else if (watch.stalled()) {
            error = std::format("no progress after popping {} items", n);
        } else {
            std::this_thread::yield();
        }

        // Before the child's first write, the value is all zeros.
        const std::optional<Value> value = reader.try_read();
        if (value && *value != Value{}) {
            if (!is_consistent(*value)) {
                error = std::format("torn value, starting as write {}", value->words[0]);
            } else if (value->words[0] < newest) {
                error = std::format("read write {} after write {}", value->words[0], newest);
            } else {
                newest = value->words[0];
            }
        }
    }
    CAPTURE(error);
    REQUIRE(error.empty());
    REQUIRE(child.wait() == 0);
    REQUIRE(reader.try_read() == make_value(kItems - 1));
}

TEST_CASE("Segments: a read-only mapping can't be written", "[segments]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());
    std::expected<SegmentFactory, std::error_code> factory = SegmentFactory::open(dir.path());
    REQUIRE(factory.has_value());
    const std::expected<Fd, std::error_code> fd =
        factory->create(SegmentKind::Track, kVersion, 2 * page_size());
    REQUIRE(fd.has_value());

    const std::expected<Region, MapFailure> writer =
        Region::map(fd->get(), SegmentKind::Track, kVersion, Access::ReadWrite);
    const std::expected<Region, MapFailure> reader =
        Region::map(fd->get(), SegmentKind::Track, kVersion, Access::ReadOnly);
    REQUIRE(writer.has_value());
    REQUIRE(reader.has_value());
    writer->bytes()[kSegmentHeaderSize] = std::byte{0x5A};
    REQUIRE(reader->bytes()[kSegmentHeaderSize] == std::byte{0x5A});

    // A child maps the segment read-only and writes through the mapping. It
    // should crash before it gets to _exit(0).
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // Die of the signal, rather than letting Catch2's handler report a
        // failure from inside the child.
        static_cast<void>(std::signal(SIGSEGV, SIG_DFL));
        static_cast<void>(std::signal(SIGBUS, SIG_DFL));
        const std::expected<Region, MapFailure> mine =
            Region::map(fd->get(), SegmentKind::Track, kVersion, Access::ReadOnly);
        if (!mine) {
            ::_exit(2);
        }
        std::ranges::fill(mine->bytes().subspan(kSegmentHeaderSize, 1), std::byte{0x11});
        ::_exit(0);
    }
    Child child{pid};
    REQUIRE(child.wait() != 0);
    REQUIRE(reader->bytes()[kSegmentHeaderSize] == std::byte{0x5A});
}

TEST_CASE("Segments: mapping checks what it maps", "[segments]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());
    std::expected<SegmentFactory, std::error_code> factory = SegmentFactory::open(dir.path());
    REQUIRE(factory.has_value());
    const std::size_t page = page_size();
    const std::expected<Fd, std::error_code> fd =
        factory->create(SegmentKind::CommandRing, kVersion, page);
    REQUIRE(fd.has_value());

    REQUIRE(refusal(fd->get(), SegmentKind::CommandRing, kVersion) == std::nullopt);
    REQUIRE(refusal(fd->get(), SegmentKind::Track, kVersion) == MapError::BadKind);
    REQUIRE(refusal(fd->get(), SegmentKind::CommandRing, kVersion + 1) == MapError::BadVersion);

    {
        // Damage the header through a writable mapping, as a buggy process
        // could, and put it back.
        const std::expected<Region, MapFailure> writable =
            Region::map(fd->get(), SegmentKind::CommandRing, kVersion, Access::ReadWrite);
        REQUIRE(writable.has_value());
        const std::span<std::byte> bytes = writable->bytes();
        SegmentHeader original{};
        std::memcpy(&original, bytes.data(), sizeof original);
#ifdef __linux__
        const int mappings = memfd_mappings();
#endif

        SegmentHeader damaged = original;
        damaged.size += page;
        std::memcpy(bytes.data(), &damaged, sizeof damaged);
        REQUIRE(refusal(fd->get(), SegmentKind::CommandRing, kVersion) == MapError::BadSize);

        damaged = original;
        damaged.magic ^= 1U;
        std::memcpy(bytes.data(), &damaged, sizeof damaged);
        REQUIRE(refusal(fd->get(), SegmentKind::CommandRing, kVersion) == MapError::BadMagic);

        std::memcpy(bytes.data(), &original, sizeof original);
        REQUIRE(refusal(fd->get(), SegmentKind::CommandRing, kVersion) == std::nullopt);
#ifdef __linux__
        // The refused mappings left nothing behind.
        REQUIRE(memfd_mappings() == mappings);
#endif
    }

    // Smaller than a header.
#ifdef __linux__
    {
        const Fd small = unsealed_memfd(64);
        REQUIRE(small.valid());
        REQUIRE(add_seals(small.get(), F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL));
        REQUIRE(refusal(small.get(), SegmentKind::CommandRing, kVersion) == MapError::TooSmall);
    }
#else
    {
        const std::filesystem::path path = dir.path() / "small";
        {
            std::ofstream file{path, std::ios::binary};
            file << std::string(64, 'x');
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX declares open() variadic
        const Fd small{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
        REQUIRE(small.valid());
        REQUIRE(refusal(small.get(), SegmentKind::CommandRing, kVersion) == MapError::TooSmall);
    }
#endif

    // Not a segment at all.
    {
        std::array<int, 2> ends{-1, -1};
        REQUIRE(::pipe(ends.data()) == 0);
        const Fd read_end{ends[0]};
        const Fd write_end{ends[1]};
        REQUIRE(refusal(read_end.get(), SegmentKind::CommandRing, kVersion) != std::nullopt);
    }

#ifdef __linux__
    // A memfd with a valid header is refused until it carries all three seals.
    {
        const Fd memfd = unsealed_memfd(page);
        REQUIRE(memfd.valid());
        void* const data =
            ::mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_SHARED, memfd.get(), 0);
        REQUIRE(data != MAP_FAILED);
        ks::ipc::write_header(std::span{static_cast<std::byte*>(data), page},
                              SegmentKind::CommandRing, kVersion);
        REQUIRE(::munmap(data, page) == 0);

        REQUIRE(refusal(memfd.get(), SegmentKind::CommandRing, kVersion) == MapError::NotSealed);
        REQUIRE(add_seals(memfd.get(), F_SEAL_SHRINK | F_SEAL_GROW));
        REQUIRE(refusal(memfd.get(), SegmentKind::CommandRing, kVersion) == MapError::NotSealed);
        REQUIRE(add_seals(memfd.get(), F_SEAL_SEAL));
        REQUIRE(refusal(memfd.get(), SegmentKind::CommandRing, kVersion) == std::nullopt);
    }
#endif
}

TEST_CASE("Segments: can't change size", "[segments]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());
    std::expected<SegmentFactory, std::error_code> factory = SegmentFactory::open(dir.path());
    REQUIRE(factory.has_value());
    const std::size_t page = page_size();
    const std::expected<Fd, std::error_code> fd =
        factory->create(SegmentKind::Track, kVersion, 2 * page);
    REQUIRE(fd.has_value());

    REQUIRE(::ftruncate(fd->get(), static_cast<off_t>(3 * page)) == -1);
    REQUIRE(::ftruncate(fd->get(), static_cast<off_t>(page)) == -1);
    struct stat status{};
    REQUIRE(::fstat(fd->get(), &status) == 0);
    REQUIRE(static_cast<std::size_t>(status.st_size) == 2 * page);

#ifdef __linux__
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX declares fcntl() variadic
    const int seals = ::fcntl(fd->get(), F_GET_SEALS);
    REQUIRE(seals != -1);
    REQUIRE((seals & F_SEAL_SHRINK) != 0);
    REQUIRE((seals & F_SEAL_GROW) != 0);
    REQUIRE((seals & F_SEAL_SEAL) != 0);
#endif
}

TEST_CASE("Segments: a segment outlives its creator", "[segments]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());
    std::expected<std::pair<Fd, Fd>, std::error_code> sockets = socket_pair();
    REQUIRE(sockets.has_value());
    const std::size_t page = page_size();

    // The child creates the segment, fills a page of it, sends it, and waits
    // to be killed.
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        sockets->first.reset();
        std::expected<SegmentFactory, std::error_code> factory = SegmentFactory::open(dir.path());
        if (!factory) {
            ::_exit(2);
        }
        const std::expected<Fd, std::error_code> fd =
            factory->create(SegmentKind::Track, kVersion, 2 * page);
        if (!fd) {
            ::_exit(3);
        }
        const std::expected<Region, MapFailure> region =
            Region::map(fd->get(), SegmentKind::Track, kVersion, Access::ReadWrite);
        if (!region) {
            ::_exit(4);
        }
        std::ranges::fill(region->bytes().subspan(page), std::byte{0xC3});
        if (!send_fd(sockets->second.get(), fd->get())) {
            ::_exit(5);
        }
        for (;;) {
            ::pause();
        }
    }
    Child child{pid};
    sockets->second.reset();

    const Fd fd = receive_fd(sockets->first.get());
    REQUIRE(fd.valid());
    child.kill();

    const std::expected<Region, MapFailure> region =
        Region::map(fd.get(), SegmentKind::Track, kVersion, Access::ReadWrite);
    REQUIRE(region.has_value());
    const std::span<std::byte> filled = region->bytes().subspan(page);
    REQUIRE(std::ranges::all_of(filled, [](std::byte b) { return b == std::byte{0xC3}; }));
    std::ranges::fill(filled, std::byte{0x3C});
    REQUIRE(std::ranges::all_of(filled, [](std::byte b) { return b == std::byte{0x3C}; }));
}

TEST_CASE("Segments: track-sized segments use memory only for what's written", "[segments]") {
    constexpr std::size_t kSegments = 4;
    constexpr std::size_t kWritten = std::size_t{1} << 20U;
    constexpr std::int64_t kLimit = std::int64_t{256} << 20U;
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());
    const std::size_t track_size = (std::size_t{2} << 30U) + page_size();

    std::array<int, 2> ends{-1, -1};
    REQUIRE(::pipe(ends.data()) == 0);
    Fd read_end{ends[0]};
    Fd write_end{ends[1]};

    // A fresh child, so nothing else the tests did counts.
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        read_end.reset();
        rusage before{};
        if (::getrusage(RUSAGE_SELF, &before) != 0) {
            ::_exit(2);
        }
        std::expected<SegmentFactory, std::error_code> factory = SegmentFactory::open(dir.path());
        if (!factory) {
            ::_exit(3);
        }
        std::vector<Region> regions;
        for (std::size_t i = 0; i < kSegments; ++i) {
            const std::expected<Fd, std::error_code> fd =
                factory->create(SegmentKind::Track, kVersion, track_size);
            if (!fd) {
                ::_exit(4);
            }
            std::expected<Region, MapFailure> region =
                Region::map(fd->get(), SegmentKind::Track, kVersion, Access::ReadWrite);
            if (!region || region->bytes().size() < track_size) {
                ::_exit(5);
            }
            std::ranges::fill(region->bytes().subspan(kSegmentHeaderSize, kWritten), std::byte{1});
            regions.push_back(std::move(*region));
        }
        rusage after{};
        if (::getrusage(RUSAGE_SELF, &after) != 0) {
            ::_exit(6);
        }
#ifdef __APPLE__
        constexpr std::int64_t kMaxRssUnit = 1; // bytes
#else
        constexpr std::int64_t kMaxRssUnit = 1024; // kibibytes
#endif
        const std::int64_t growth = (max_rss(after) - max_rss(before)) * kMaxRssUnit;
        if (::write(write_end.get(), &growth, sizeof growth) != sizeof growth) {
            ::_exit(7);
        }
        ::_exit(0);
    }
    Child child{pid};
    write_end.reset();

    std::int64_t growth = -1;
    const ssize_t got = ::read(read_end.get(), &growth, sizeof growth);
    const int exit_code = child.wait();
    CAPTURE(exit_code, growth / (std::int64_t{1} << 20U));
    REQUIRE(exit_code == 0);
    REQUIRE(got == sizeof growth);
    REQUIRE(growth < kLimit);
}

TEST_CASE("Segments: no names are left behind", "[segments]") {
    const TempDir dir;
    REQUIRE_FALSE(dir.path().empty());
    const std::filesystem::path intent = dir.path() / ks::ipc::kIntentFileName;

#ifdef __APPLE__
    {
        std::expected<SegmentFactory, std::error_code> factory = SegmentFactory::open(dir.path());
        REQUIRE(factory.has_value());
        REQUIRE(factory->create(SegmentKind::Track, kVersion, page_size()).has_value());
        REQUIRE(std::filesystem::exists(intent));
        REQUIRE(std::filesystem::file_size(intent) == 0);
    }

    // A name a crash left behind is unlinked.
    const std::string leftover = std::string{ks::ipc::kSegmentNamePrefix} + random_letters(16);
    REQUIRE(leave_object(leftover));
    write_file(intent, leftover);
    {
        const std::expected<SegmentFactory, std::error_code> factory =
            SegmentFactory::open(dir.path());
        REQUIRE(factory.has_value());
    }
    REQUIRE_FALSE(object_exists(leftover));
    REQUIRE(std::filesystem::file_size(intent) == 0);

    // A name of another form is left alone, and so is garbage.
    const std::string other = "/ks-test-" + random_letters(8);
    REQUIRE(leave_object(other));
    for (const std::string& contents :
         {other, std::string{"garbage\n"},
          std::string{ks::ipc::kSegmentNamePrefix} + "UPPER-case!!!!!!"}) {
        CAPTURE(contents);
        write_file(intent, contents);
        const std::expected<SegmentFactory, std::error_code> factory =
            SegmentFactory::open(dir.path());
        REQUIRE(factory.has_value());
        REQUIRE(std::filesystem::file_size(intent) == 0);
    }
    REQUIRE(object_exists(other));
    REQUIRE(::shm_unlink(other.c_str()) == 0);

    // One factory per runtime directory.
    {
        const std::expected<SegmentFactory, std::error_code> first =
            SegmentFactory::open(dir.path());
        REQUIRE(first.has_value());
        const std::expected<SegmentFactory, std::error_code> second =
            SegmentFactory::open(dir.path());
        REQUIRE_FALSE(second.has_value());
        REQUIRE(second.error() == std::errc::device_or_resource_busy);
    }
    REQUIRE(SegmentFactory::open(dir.path()).has_value());
#else
    std::expected<SegmentFactory, std::error_code> factory = SegmentFactory::open(dir.path());
    REQUIRE(factory.has_value());
    REQUIRE(factory->create(SegmentKind::Track, kVersion, page_size()).has_value());
    REQUIRE_FALSE(std::filesystem::exists(intent));
    REQUIRE(std::filesystem::is_empty(dir.path()));
    REQUIRE(SegmentFactory::open(dir.path()).has_value());
#endif
}
