// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Helpers for specs/logs/spec.md's acceptance tests: reading log files and
// lines, the shape every line must have, starting ks-engine with a log
// segment and kool_spin with its standard error in a file, and the checks
// the record fuzz target makes, which "Log lines: format" also runs.

#include <ks/ipc/fd.h>
#include <ks/ipc/layout.h>
#include <ks/ipc/log.h>
#include <ks/ipc/region.h>
#include <ks/ipc/segment.h>
#include <ks/supervisor/spawn.h>

#include "record_check.h"
#include "rings_support.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ks::logs::test {

using processes::test::Clock;
using processes::test::eventually;
using processes::test::Milliseconds;
using processes::test::Process;

/// How long a test waits for a line to reach a file: the supervisor reads
/// the engine's ring at least every 100 ms.
inline constexpr Milliseconds kLineWait{2'000};

/// Nanoseconds of std::chrono::steady_clock now, as log records have them.
inline std::uint64_t steady_now() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

// ---- The shape of lines ----------------------------------------------------------

/// A line's time, level, and text.
struct Line {
    std::string time; ///< "YYYY-MM-DD HH:MM:SS.ffffff": comparable as text.
    std::string level;
    std::string text;
};

/// The pattern every line but the header matches ("A well-formed file").
inline const std::regex& line_pattern() {
    static const std::regex kPattern{
        R"(^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6}) (error|warning|info) (.*)$)"};
    return kPattern;
}

/// `line` split into its parts, or nothing if it isn't shaped like a line.
inline std::optional<Line> parse_line(const std::string& line) {
    std::smatch match;
    if (!std::regex_match(line, match, line_pattern())) {
        return std::nullopt;
    }
    return Line{.time = match[1].str(), .level = match[2].str(), .text = match[3].str()};
}

/// The header pattern for `process` (requirement 7). Group 1 is everything
/// after "log; ", which every file in a session shares.
inline std::regex header_pattern(std::string_view process) {
    return std::regex{
        "^# " + std::string{process} +
        R"( log; (session started \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6} [+-]\d{2}:\d{2}; )"
        R"(steady clock \d+\.\d{9})$)"};
}

/// The part of `header` after "log; ", or nothing if it isn't a header for
/// `process`.
inline std::optional<std::string> header_tail(const std::string& header, std::string_view process) {
    std::smatch match;
    const std::regex pattern = header_pattern(process);
    if (!std::regex_match(header, match, pattern)) {
        return std::nullopt;
    }
    return match[1].str();
}

/// The lines of `file`, without their newlines. Empty if it can't be read.
inline std::vector<std::string> read_lines(const std::filesystem::path& file) {
    std::vector<std::string> lines;
    std::ifstream in{file, std::ios::binary};
    for (std::string line; std::getline(in, line);) {
        lines.push_back(std::move(line));
    }
    return lines;
}

/// True if `lines` are a well-formed file for `process`: a header first,
/// then lines that each match the line pattern and are clean.
inline bool well_formed(const std::vector<std::string>& lines, std::string_view process) {
    if (lines.empty() || !header_tail(lines.front(), process)) {
        return false;
    }
    return std::ranges::all_of(std::span{lines}.subspan(1), [](const std::string& line) {
        return clean(line) && parse_line(line).has_value();
    });
}

/// The first line in `lines` at `level` whose text contains `phrase`.
inline std::optional<Line> find_line(const std::vector<std::string>& lines, std::string_view level,
                                     std::string_view phrase) {
    for (const std::string& raw : lines) {
        std::optional<Line> line = parse_line(raw);
        if (line && line->level == level && line->text.find(phrase) != std::string::npos) {
            return line;
        }
    }
    return std::nullopt;
}

/// Waits up to kLineWait for `file` to have a line at `level` containing
/// `phrase`, and returns it.
inline std::optional<Line> wait_for_line(const std::filesystem::path& file, std::string_view level,
                                         std::string_view phrase) {
    std::optional<Line> found;
    static_cast<void>(eventually(
        [&] {
            found = find_line(read_lines(file), level, phrase);
            return found.has_value();
        },
        kLineWait));
    return found;
}

// ---- Session folders -------------------------------------------------------------

/// True if `name` has the session folder pattern (requirement 10).
inline bool session_name(const std::string& name) {
    static const std::regex kPattern{R"(^\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}_\d+$)"};
    return std::regex_match(name, kPattern);
}

/// The folders in `log_dir` whose names have the session pattern, sorted by
/// name.
inline std::vector<std::filesystem::path> session_folders(const std::filesystem::path& log_dir) {
    std::vector<std::filesystem::path> folders;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{log_dir, error}) {
        if (entry.is_directory() && session_name(entry.path().filename().string())) {
            folders.push_back(entry.path());
        }
    }
    std::ranges::sort(folders);
    return folders;
}

/// The permission bits of `path`, or nothing if it doesn't exist.
inline std::optional<unsigned> mode_of(const std::filesystem::path& path) {
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0) {
        return std::nullopt;
    }
    return static_cast<unsigned>(status.st_mode) & 0777U;
}

/// Writes `text` to a new file at `path`.
inline void write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out{path, std::ios::binary};
    out << text;
    REQUIRE(out.good());
}

// ---- Records -------------------------------------------------------------------

/// A record's text, with its length clamped to the record.
inline std::string text_of(const ipc::LogRecord& record) {
    const std::size_t length = std::min<std::uint64_t>(record.length, ipc::kLogTextSize);
    const std::span<const std::uint8_t> bytes = std::span{record.text}.first(length);
    return std::string{bytes.begin(), bytes.end()};
}

/// A record with `text`, at `time`, `level`, and `flags`, as a writer would
/// make it, but with nothing checked.
inline ipc::LogRecord make_record(std::uint64_t time, std::uint64_t level, std::string_view text,
                                  std::uint64_t flags = 0) {
    ipc::LogRecord record{.time = time, .level = level, .length = 0, .flags = flags, .text = {}};
    const std::size_t length = std::min(text.size(), ipc::kLogTextSize);
    const std::span<std::uint8_t> bytes{record.text};
    for (std::size_t i = 0; i < length; ++i) {
        bytes[i] = static_cast<std::uint8_t>(text[i]);
    }
    record.length = length;
    return record;
}

/// A zero-filled log segment in private memory: an empty log.
inline std::unique_ptr<ipc::LogSegment> private_segment() {
    return std::make_unique<ipc::LogSegment>();
}

// ---- ks-engine with a log segment --------------------------------------------------

/// The arguments for /bin/sh that run `program` with `args`, with its
/// standard error appended to `err`.
inline std::vector<std::string> with_stderr(const std::filesystem::path& err,
                                            std::string_view program,
                                            const std::vector<std::string>& args) {
    std::vector<std::string> shell{"-c", R"(exec "$@" 2>>"$0")", err.string(),
                                   std::string{program}};
    shell.insert(shell.end(), args.begin(), args.end());
    return shell;
}

/// A log segment, and the test's read-write mapping of it.
struct MappedLog {
    ipc::Fd fd;
    ipc::Region region;
    ipc::LogSegment* segment = nullptr;
};

/// Creates a log segment as spec requirement 1 says, and maps it.
inline MappedLog create_log(ipc::SegmentFactory& factory) {
    std::expected<ipc::Fd, std::error_code> fd =
        factory.create(ipc::SegmentKind::Log, ipc::kLogLayoutVersion, ipc::kLogSegmentSize);
    REQUIRE(fd.has_value());
    std::expected<ipc::Region, ipc::MapFailure> region = ipc::Region::map(
        fd->get(), ipc::SegmentKind::Log, ipc::kLogLayoutVersion, ipc::Access::ReadWrite);
    REQUIRE(region.has_value());
    auto* const segment = region->at<ipc::LogSegment>(ipc::kSegmentHeaderSize);
    REQUIRE(segment != nullptr);
    return MappedLog{.fd = std::move(*fd), .region = std::move(*region), .segment = segment};
}

/// ks-engine with the test as its supervisor, as client_rings::test::
/// Supervised, plus a log segment and a file for its standard error.
struct LoggedEngine {
    std::unique_ptr<client_rings::test::Supervised> supervised;
    std::unique_ptr<MappedLog> log;
    std::filesystem::path err;
};

/// The parts of a supervised engine that exist before it starts: the runtime
/// directory, the segment factory, the state segment and its view, and a log
/// segment.
inline std::unique_ptr<LoggedEngine> prepare_engine() {
    auto engine = std::make_unique<LoggedEngine>();
    engine->supervised = std::make_unique<client_rings::test::Supervised>();
    client_rings::test::Supervised& supervised = *engine->supervised;
    REQUIRE(!supervised.dir.path().empty());
    std::expected<ipc::SegmentFactory, std::error_code> factory =
        ipc::SegmentFactory::open(supervised.dir.path());
    REQUIRE(factory.has_value());
    supervised.factory = std::make_unique<ipc::SegmentFactory>(std::move(*factory));
    std::expected<ipc::Fd, std::error_code> state =
        supervised.factory->create(ipc::SegmentKind::EngineState, ipc::kEngineStateLayoutVersion,
                                   ipc::kEngineStateSegmentSize);
    REQUIRE(state.has_value());
    supervised.state_fd = std::move(*state);
    std::expected<client_rings::test::EngineView, std::string> view =
        client_rings::test::EngineView::map(supervised.state_fd.get());
    REQUIRE(view.has_value());
    supervised.state = std::make_unique<client_rings::test::EngineView>(std::move(*view));
    engine->log = std::make_unique<MappedLog>(create_log(*supervised.factory));
    engine->err = supervised.dir.path() / "stderr";
    return engine;
}

/// The arguments kool_spin would give ks-engine, with `log_args` after the
/// descriptors (for example {"--log-fd", "5"}), then the null backend.
inline std::vector<std::string> engine_args(const LoggedEngine& engine,
                                            const std::vector<std::string>& log_args) {
    std::vector<std::string> args{"--supervisor-fd", "3", "--state-fd", "4"};
    args.insert(args.end(), log_args.begin(), log_args.end());
    args.insert(args.end(), {"--runtime-dir", engine.supervised->dir.path().string()});
    const std::vector<std::string> options = client_rings::test::output_options();
    args.insert(args.end(), options.begin(), options.end());
    return args;
}

/// Starts ks-engine through /bin/sh, with its standard error appended to
/// `engine.err`, the socket as descriptor 3, the state segment as 4, and the
/// log segment as 5. Returns the test's end of the socket, non-blocking.
inline std::unique_ptr<ipc::Connection> launch_engine(LoggedEngine& engine,
                                                      const std::vector<std::string>& args) {
    std::expected<std::pair<ipc::Fd, ipc::Fd>, std::error_code> pair = ipc::socket_pair();
    REQUIRE(pair.has_value());
    const std::vector<supervisor::Inherit> fds{
        {.parent = pair->second.get(), .child = 3},
        {.parent = engine.supervised->state_fd.get(), .child = 4},
        {.parent = engine.log->fd.get(), .child = 5}};
    engine.supervised->engine = processes::test::start(
        "/bin/sh", with_stderr(engine.err, processes::test::kEnginePath, args), fds);
    REQUIRE(engine.supervised->engine.pid() > 0);
    pair->second.reset();
    REQUIRE(ipc::test::set_nonblocking(pair->first.get()));
    return std::make_unique<ipc::Connection>(std::move(pair->first));
}

/// Starts ks-engine with `--log-fd 5`, and waits for its Ready.
inline std::unique_ptr<LoggedEngine> start_logged_engine() {
    std::unique_ptr<LoggedEngine> engine = prepare_engine();
    engine->supervised->socket = launch_engine(*engine, engine_args(*engine, {"--log-fd", "5"}));
    REQUIRE(processes::test::is<ipc::msg::Ready>(
        processes::test::receive(*engine->supervised->socket), 0));
    return engine;
}

// ---- kool_spin with logs -----------------------------------------------------------

/// kool_spin in a runtime directory of its own, with ks-engine on the null
/// backend, started through /bin/sh so that its standard error goes to a
/// file. Its log folder is `<dir>/logs` unless the test gives another, or
/// none at all (the default folder). Whatever happens, kool_spin and any
/// engine left without it are killed when this is destroyed.
class LoggedKoolSpin {
public:
    /// Doesn't start anything yet: tests prepare the log folder first.
    LoggedKoolSpin() { REQUIRE(!dir_.path().empty()); }

    LoggedKoolSpin(const LoggedKoolSpin&) = delete;
    LoggedKoolSpin& operator=(const LoggedKoolSpin&) = delete;
    LoggedKoolSpin(LoggedKoolSpin&&) = delete;
    LoggedKoolSpin& operator=(LoggedKoolSpin&&) = delete;
    ~LoggedKoolSpin() = default;

    /// Starts kool_spin with `--log-dir <log_dir>`, or with no --log-dir if
    /// `log_dir` is empty, and waits up to 5 s for supervisor.sock. With
    /// `env`, kool_spin runs through /usr/bin/env with those arguments
    /// first, such as "-u NAME" and "NAME=value", so the test never changes
    /// its own environment.
    void start(const std::optional<std::filesystem::path>& log_dir,
               const std::vector<std::string>& env = {}) {
        std::vector<std::string> args{"--runtime-dir", dir_.path().string()};
        if (log_dir) {
            args.insert(args.end(), {"--log-dir", log_dir->string()});
        }
        args.insert(args.end(), {"--engine", std::string{processes::test::kEnginePath}, "--"});
        const std::vector<std::string> options = processes::test::null_backend();
        args.insert(args.end(), options.begin(), options.end());
        std::string_view program = processes::test::kKoolSpinPath;
        if (!env.empty()) {
            std::vector<std::string> with_env = env;
            with_env.emplace_back(processes::test::kKoolSpinPath);
            with_env.insert(with_env.end(), args.begin(), args.end());
            args = std::move(with_env);
            program = "/usr/bin/env";
        }
        process_ = processes::test::start("/bin/sh", with_stderr(err(), program, args));
        REQUIRE(process_.pid() > 0);
        const std::filesystem::path socket = dir_.path() / "supervisor.sock";
        static_cast<void>(
            eventually([&] { return std::filesystem::exists(socket) || !process_.running(); }));
        REQUIRE(std::filesystem::exists(socket));
        REQUIRE(process_.running());
    }

    /// Starts kool_spin with its log folder at `<dir>/logs`.
    void start() { start(logs()); }

    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_.path(); }
    [[nodiscard]] std::filesystem::path logs() const { return dir_.path() / "logs"; }
    [[nodiscard]] std::filesystem::path err() const { return dir_.path() / "stderr"; }
    [[nodiscard]] Process& process() noexcept { return process_; }

    /// The engine's process ID, from its lock; 0 if none is running.
    [[nodiscard]] pid_t engine() const {
        return processes::test::lock_holder(dir_.path(), ipc::kEngineLockName).value_or(0);
    }

    /// The one session folder in `log_dir`, waiting up to 5 s for it.
    [[nodiscard]] std::optional<std::filesystem::path>
    session(const std::filesystem::path& log_dir) const {
        std::optional<std::filesystem::path> found;
        static_cast<void>(eventually([&] {
            for (const std::filesystem::path& folder : session_folders(log_dir)) {
                if (folder.filename().string().ends_with("_" + std::to_string(process_.pid()))) {
                    found = folder;
                }
            }
            return found.has_value();
        }));
        return found;
    }

    /// The session folder in `<dir>/logs`.
    [[nodiscard]] std::optional<std::filesystem::path> session() const { return session(logs()); }

private:
    ipc::test::TempDir dir_;
    processes::test::EngineReaper reaper_{dir_.path()};
    Process process_;
};

} // namespace ks::logs::test
