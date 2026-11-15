// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/segment.h>

#include <ks/ipc/fd.h>
#include <ks/ipc/region.h>

#include "posix.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef __APPLE__
#include <cstdlib> // arc4random_buf
#endif

namespace ks::ipc {

namespace {

using detail::last_error;
using detail::retry_eintr;

/// `size` rounded up to the page size.
std::expected<std::size_t, std::error_code> round_up_to_page(std::size_t size) {
    if (size == 0) {
        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    }
    const long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        return std::unexpected{last_error()};
    }
    const auto page_size = static_cast<std::size_t>(page);
    if (size > std::numeric_limits<std::size_t>::max() - (page_size - 1)) {
        return std::unexpected{std::make_error_code(std::errc::value_too_large)};
    }
    const std::size_t rounded = (size + page_size - 1) / page_size * page_size;
    if (rounded > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
        return std::unexpected{std::make_error_code(std::errc::value_too_large)};
    }
    return rounded;
}

/// Maps all `size` bytes of `fd`, writes a header recording that size, and
/// unmaps them again. Only the header's page is touched.
std::expected<void, std::error_code>
write_segment_header(int fd, std::size_t size, SegmentKind kind, std::uint32_t layout_version) {
    void* const data = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        return std::unexpected{last_error()};
    }
    write_header(std::span{static_cast<std::byte*>(data), size}, kind, layout_version);
    static_cast<void>(::munmap(data, size));
    return {};
}

std::expected<void, std::error_code> resize(int fd, std::size_t size) {
    if (retry_eintr([&] { return ::ftruncate(fd, static_cast<off_t>(size)); }) == -1) {
        return std::unexpected{last_error()};
    }
    return {};
}

MapError to_map_error(HeaderError error) noexcept {
    switch (error) {
    case HeaderError::TooSmall:
        return MapError::TooSmall;
    case HeaderError::BadMagic:
        return MapError::BadMagic;
    case HeaderError::BadVersion:
        return MapError::BadVersion;
    case HeaderError::BadKind:
        return MapError::BadKind;
    case HeaderError::BadSize:
        return MapError::BadSize;
    }
    return MapError::BadMagic;
}

#ifdef __linux__

/// A sealed memfd (requirement 8). Linux needs no intent file.
std::expected<Fd, std::error_code> create_segment(const Fd& /*intent*/, SegmentKind kind,
                                                  std::uint32_t layout_version, std::size_t size) {
    Fd fd{::memfd_create("kool_spin", MFD_CLOEXEC | MFD_ALLOW_SEALING)};
    if (!fd.valid()) {
        return std::unexpected{last_error()};
    }
    if (auto resized = resize(fd.get(), size); !resized) {
        return std::unexpected{resized.error()};
    }
    if (auto written = write_segment_header(fd.get(), size, kind, layout_version); !written) {
        return std::unexpected{written.error()};
    }
    if (detail::fcntl_int(fd.get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) == -1 ||
        detail::fcntl_int(fd.get(), F_ADD_SEALS, F_SEAL_SEAL) == -1) {
        return std::unexpected{last_error()};
    }
    return fd;
}

#endif

#ifdef __APPLE__

constexpr std::size_t kNameRandomLength = 16;
constexpr std::string_view kNameAlphabet = "abcdefghijklmnopqrstuvwxyz0123456789";

/// True if `name` has the form create() gives names (requirement 9).
bool is_segment_name(std::string_view name) noexcept {
    if (name.size() != kSegmentNamePrefix.size() + kNameRandomLength ||
        !name.starts_with(kSegmentNamePrefix)) {
        return false;
    }
    for (const char c : name.substr(kSegmentNamePrefix.size())) {
        if (kNameAlphabet.find(c) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

/// A random name of the form is_segment_name() accepts.
std::string random_segment_name() {
    std::array<unsigned char, kNameRandomLength> random{};
    ::arc4random_buf(random.data(), random.size());
    std::string name{kSegmentNamePrefix};
    for (const unsigned char byte : random) {
        name += kNameAlphabet[byte % kNameAlphabet.size()];
    }
    return name;
}

/// Replaces the intent file's contents with `name` (empty: clears it).
std::expected<void, std::error_code> write_intent(int intent, std::string_view name) {
    if (auto cleared = resize(intent, 0); !cleared) {
        return cleared;
    }
    if (name.empty()) {
        return {};
    }
    const ssize_t written =
        retry_eintr([&] { return ::pwrite(intent, name.data(), name.size(), 0); });
    if (written == -1) {
        return std::unexpected{last_error()};
    }
    if (static_cast<std::size_t>(written) != name.size()) {
        return std::unexpected{std::make_error_code(std::errc::io_error)};
    }
    return {};
}

/// shm_open(), whose mode argument POSIX declares variadic.
int open_shared_memory(const char* name, int flags, mode_t mode) noexcept {
    return retry_eintr([&] {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): shm_open() is variadic
        return ::shm_open(name, flags, mode);
    });
}

/// Unlinks the object `name` and clears the intent file, after a failure
/// once the object exists (requirement 10).
void abandon(int intent, const std::string& name) noexcept {
    static_cast<void>(::shm_unlink(name.c_str()));
    static_cast<void>(write_intent(intent, {}));
}

/// An unlinked POSIX shared-memory object (requirements 9 and 10).
std::expected<Fd, std::error_code> create_segment(const Fd& intent_file, SegmentKind kind,
                                                  std::uint32_t layout_version, std::size_t size) {
    const int intent = intent_file.get();
    for (int attempt = 0; attempt < kMaxSegmentNameAttempts; ++attempt) {
        const std::string name = random_segment_name();
        if (auto recorded = write_intent(intent, name); !recorded) {
            return std::unexpected{recorded.error()};
        }
        Fd fd{open_shared_memory(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600)};
        if (!fd.valid()) {
            const std::error_code error = last_error();
            static_cast<void>(write_intent(intent, {}));
            if (error == std::errc::file_exists) {
                continue;
            }
            return std::unexpected{error};
        }
        if (!detail::set_cloexec(fd.get())) {
            const std::error_code error = last_error();
            abandon(intent, name);
            return std::unexpected{error};
        }
        if (auto resized = resize(fd.get(), size); !resized) {
            abandon(intent, name);
            return std::unexpected{resized.error()};
        }
        if (::shm_unlink(name.c_str()) == -1) {
            // The name stays in the intent file, for the next open() to unlink.
            return std::unexpected{last_error()};
        }
        if (auto cleared = write_intent(intent, {}); !cleared) {
            return std::unexpected{cleared.error()};
        }
        if (auto written = write_segment_header(fd.get(), size, kind, layout_version); !written) {
            return std::unexpected{written.error()};
        }
        return fd;
    }
    return std::unexpected{std::make_error_code(std::errc::file_exists)};
}

/// Unlinks a name a crash left in the intent file, then clears the file
/// (requirement 11).
std::expected<void, std::error_code> recover_intent(int intent) {
    // A valid name is 27 bytes, so a full buffer means something else.
    std::array<char, 64> contents{};
    const ssize_t read =
        retry_eintr([&] { return ::pread(intent, contents.data(), contents.size(), 0); });
    if (read == -1) {
        return std::unexpected{last_error()};
    }
    const std::string name{contents.data(), static_cast<std::size_t>(read)};
    if (is_segment_name(name) && ::shm_unlink(name.c_str()) == -1 && errno != ENOENT) {
        return std::unexpected{last_error()};
    }
    return write_intent(intent, {});
}

#endif

} // namespace

std::expected<SegmentFactory, std::error_code>
SegmentFactory::open(const std::filesystem::path& runtime_dir) {
    struct stat status{};
    if (::stat(runtime_dir.c_str(), &status) == -1) {
        return std::unexpected{last_error()};
    }
    if (!S_ISDIR(status.st_mode)) {
        return std::unexpected{std::make_error_code(std::errc::not_a_directory)};
    }
#ifdef __APPLE__
    const std::filesystem::path path = runtime_dir / kIntentFileName;
    Fd intent{detail::open_file(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (!intent.valid()) {
        return std::unexpected{last_error()};
    }
    if (retry_eintr([&] { return ::flock(intent.get(), LOCK_EX | LOCK_NB); }) == -1) {
        if (errno == EWOULDBLOCK) {
            return std::unexpected{std::make_error_code(std::errc::device_or_resource_busy)};
        }
        return std::unexpected{last_error()};
    }
    if (auto recovered = recover_intent(intent.get()); !recovered) {
        return std::unexpected{recovered.error()};
    }
    return SegmentFactory{std::move(intent)};
#else
    return SegmentFactory{Fd{}};
#endif
}

std::expected<Fd, std::error_code>
SegmentFactory::create(SegmentKind kind, std::uint32_t layout_version, std::size_t size) {
    const std::expected<std::size_t, std::error_code> rounded = round_up_to_page(size);
    if (!rounded) {
        return std::unexpected{rounded.error()};
    }
    return create_segment(intent_, kind, layout_version, *rounded);
}

std::expected<Region, MapFailure> Region::map(int fd, SegmentKind kind,
                                              std::uint32_t layout_version, Access access) {
#ifdef __linux__
    constexpr int kRequiredSeals = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;
    const int seals = detail::fcntl_get(fd, F_GET_SEALS);
    if (seals == -1) {
        if (errno == EINVAL) {
            // Not a file that can carry seals.
            return std::unexpected{MapFailure{.error = MapError::NotSealed, .system = {}}};
        }
        return std::unexpected{MapFailure{.error = MapError::System, .system = last_error()}};
    }
    if ((seals & kRequiredSeals) != kRequiredSeals) {
        return std::unexpected{MapFailure{.error = MapError::NotSealed, .system = {}}};
    }
#endif
    struct stat status{};
    if (::fstat(fd, &status) == -1) {
        return std::unexpected{MapFailure{.error = MapError::System, .system = last_error()}};
    }
    if (std::cmp_less(status.st_size, kSegmentHeaderSize)) {
        return std::unexpected{MapFailure{.error = MapError::TooSmall, .system = {}}};
    }
    if (std::cmp_greater(status.st_size, std::numeric_limits<std::size_t>::max())) {
        return std::unexpected{MapFailure{
            .error = MapError::System, .system = std::make_error_code(std::errc::value_too_large)}};
    }
    const auto size = static_cast<std::size_t>(status.st_size);
    const int protection = access == Access::ReadOnly ? PROT_READ : PROT_READ | PROT_WRITE;
    void* const data = ::mmap(nullptr, size, protection, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        return std::unexpected{MapFailure{.error = MapError::System, .system = last_error()}};
    }
    Region region{static_cast<std::byte*>(data), size};
    if (auto checked = check_header(region.bytes(), kind, layout_version); !checked) {
        return std::unexpected{MapFailure{.error = to_map_error(checked.error()), .system = {}}};
    }
    return region;
}

} // namespace ks::ipc
