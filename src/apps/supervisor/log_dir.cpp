// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/supervisor/log_dir.h>

#include <ks/ipc/fd.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace ks::supervisor {

namespace {

constexpr mode_t kDirMode = 0700;

/// `YYYY-MM-DD_HH-MM-SS`, then `_` and the process ID.
constexpr std::string_view kTimePattern = "dddd-dd-dd_dd-dd-dd";
constexpr std::size_t kMaxPidDigits = 19; // Any 19 digits fit a std::uint64_t.

constexpr std::int64_t kSecondNs = 1'000'000'000;
constexpr std::int64_t kDaySeconds = 86'400;
/// 10000-01-01 00:00:00, in seconds since 1970.
constexpr std::int64_t kYear10000 = 253'402'300'800;

constexpr bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

std::error_code last_error() noexcept {
    return std::error_code{errno, std::generic_category()};
}

std::optional<std::string_view> variable(const char* name) noexcept {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read as the supervisor starts, before it has threads.
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string_view{value};
}

std::optional<std::filesystem::path> absolute(std::optional<std::string_view> value) {
    if (!value || !value->starts_with('/')) {
        return std::nullopt;
    }
    return std::filesystem::path{*value};
}

/// Opens a folder, retrying a call that a signal interrupted.
ipc::Fd open_dir(int parent, const char* name, bool follow) noexcept {
    const int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | (follow ? 0 : O_NOFOLLOW);
    ipc::Fd fd;
    while (!fd.valid()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): openat() is the POSIX interface.
        fd.reset(::openat(parent, name, flags));
        if (!fd.valid() && errno != EINTR) {
            break;
        }
    }
    return fd;
}

/// The type of `name` in the folder `dir`, itself and not what it links to.
std::optional<mode_t> type_of(int dir, const std::string& name) noexcept {
    struct stat status{};
    if (::fstatat(dir, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        return std::nullopt;
    }
    return status.st_mode & S_IFMT;
}

/// The names in a folder; those read before an error, if there is one.
std::vector<std::string> names_in(const std::filesystem::path& path) {
    std::vector<std::string> names;
    std::error_code error;
    std::filesystem::directory_iterator entry{path, error};
    const std::filesystem::directory_iterator end;
    while (!error && entry != end) {
        names.push_back(entry->path().filename().string());
        entry.increment(error);
    }
    return names;
}

/// Deletes the regular `.log` files in the session folder `name`, then the
/// folder if it's empty. The names come from the path, but every check and
/// every deletion goes through descriptors opened without following links.
void remove_session(int log_dir, const std::filesystem::path& path, const std::string& name) {
    {
        const ipc::Fd session = open_dir(log_dir, name.c_str(), false);
        if (!session.valid()) {
            return;
        }
        for (const std::string& file : names_in(path)) {
            if (file.ends_with(".log") && type_of(session.get(), file) == mode_t{S_IFREG}) {
                // A file that can't be deleted stays, and so does its folder.
                static_cast<void>(::unlinkat(session.get(), file.c_str(), 0));
            }
        }
    }
    // Fails, as it should, if anything is left in the folder.
    static_cast<void>(::unlinkat(log_dir, name.c_str(), AT_REMOVEDIR));
}

} // namespace

std::optional<std::filesystem::path>
default_log_dir(std::optional<std::string_view> home,
                std::optional<std::string_view> xdg_state_home) {
#ifdef __APPLE__
    static_cast<void>(xdg_state_home);
    if (std::optional<std::filesystem::path> base = absolute(home)) {
        return *base / "Library" / "Logs" / "kool_spin";
    }
#else
    if (std::optional<std::filesystem::path> base = absolute(xdg_state_home)) {
        return *base / "kool_spin" / "logs";
    }
    if (std::optional<std::filesystem::path> base = absolute(home)) {
        return *base / ".local" / "state" / "kool_spin" / "logs";
    }
#endif
    return std::nullopt;
}

std::optional<std::filesystem::path> default_log_dir() {
    return default_log_dir(variable("HOME"), variable("XDG_STATE_HOME"));
}

std::error_code create_log_dir(const std::filesystem::path& path) {
    // Walk up to the nearest folder that exists, then create the rest.
    std::vector<std::filesystem::path> missing;
    std::filesystem::path current = path;
    while (::mkdir(current.c_str(), kDirMode) != 0) {
        if (errno == EEXIST) {
            std::error_code error;
            if (!std::filesystem::is_directory(current, error)) {
                return error ? error : std::make_error_code(std::errc::not_a_directory);
            }
            break;
        }
        if (errno != ENOENT) {
            return last_error();
        }
        std::filesystem::path parent = current.parent_path();
        if (parent.empty() || parent == current) {
            return std::make_error_code(std::errc::no_such_file_or_directory);
        }
        missing.push_back(std::move(current));
        current = std::move(parent);
    }
    for (const std::filesystem::path& folder : missing | std::views::reverse) {
        // Something else may have created it meanwhile, or the path may have
        // named it twice ("a/b/" after "a/b").
        if (::mkdir(folder.c_str(), kDirMode) != 0 && errno != EEXIST) {
            return last_error();
        }
    }
    return {};
}

std::optional<std::string> session_name(const ipc::LogClock& clock, pid_t pid) {
    if (pid < 0) {
        return std::nullopt;
    }
    // Checking the offset first means the sum below can't overflow.
    if (clock.utc_offset_s < -kYear10000 || clock.utc_offset_s > kYear10000) {
        return std::nullopt;
    }
    // Rounded down, for times before 1970 too.
    std::int64_t seconds = clock.system_ns / kSecondNs;
    if (clock.system_ns % kSecondNs < 0) {
        --seconds;
    }
    seconds += clock.utc_offset_s;
    if (seconds < 0 || seconds >= kYear10000) {
        return std::nullopt;
    }
    const std::int64_t day = seconds / kDaySeconds;
    const std::int64_t in_day = seconds % kDaySeconds;
    const std::chrono::year_month_day date{
        std::chrono::sys_days{std::chrono::days{static_cast<int>(day)}}};
    return std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}_{}", static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                       in_day / 3'600, in_day / 60 % 60, in_day % 60,
                       static_cast<std::int64_t>(pid));
}

std::optional<SessionName> parse_session_name(std::string_view name) noexcept {
    const std::size_t time_size = kTimePattern.size();
    if (name.size() < time_size + 2 || name.size() > time_size + 1 + kMaxPidDigits) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < time_size; ++i) {
        const bool matches =
            kTimePattern[i] == 'd' ? is_digit(name[i]) : name[i] == kTimePattern[i];
        if (!matches) {
            return std::nullopt;
        }
    }
    const std::string_view digits = name.substr(time_size + 1);
    if (name[time_size] != '_' || !std::ranges::all_of(digits, is_digit)) {
        return std::nullopt;
    }
    // At most kMaxPidDigits digits, so this can't overflow.
    std::uint64_t pid = 0;
    for (const char digit : digits) {
        pid = (pid * 10) + static_cast<std::uint64_t>(digit - '0');
    }
    return SessionName{.time = name.substr(0, time_size), .pid = pid};
}

void remove_old_sessions(const std::filesystem::path& log_dir, std::size_t keep) {
    const ipc::Fd dir = open_dir(AT_FDCWD, log_dir.c_str(), true);
    if (!dir.valid()) {
        return;
    }

    struct Session {
        std::string name;
        std::uint64_t pid = 0;
    };
    std::vector<Session> sessions;
    for (std::string& name : names_in(log_dir)) {
        const std::optional<SessionName> parsed = parse_session_name(name);
        if (parsed && type_of(dir.get(), name) == mode_t{S_IFDIR}) {
            sessions.push_back({.name = std::move(name), .pid = parsed->pid});
        }
    }
    if (sessions.size() <= keep) {
        return;
    }

    // Newest first. Two names with one time and one process ID ("_7" and
    // "_07") are ordered by name, so the choice doesn't depend on the listing.
    const auto key = [](const Session& session) {
        return std::tuple{std::string_view{session.name}.substr(0, kTimePattern.size()),
                          session.pid, std::string_view{session.name}};
    };
    std::ranges::sort(sessions,
                      [&key](const Session& a, const Session& b) { return key(a) > key(b); });
    for (const Session& session : std::span{sessions}.subspan(keep)) {
        remove_session(dir.get(), log_dir / session.name, session.name);
    }
}

} // namespace ks::supervisor
