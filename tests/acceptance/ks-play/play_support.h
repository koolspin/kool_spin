// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by specs/ks-play/spec.md's ks-play tests: running the built ks-play
// with its standard output and standard error in files, and reading what
// `status` prints. Built on the track-loading tests' helpers.

#include "unload_support.h"
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef KS_PLAY_PATH
#error "KS_PLAY_PATH must name the ks-play executable"
#endif

namespace ks::ks_play::test {

inline constexpr std::string_view kPlayPath = KS_PLAY_PATH;

using processes::test::Process;
using track_loading::test::Milliseconds;

/// The contents of `path`, or nothing if it can't be read.
inline std::string slurp(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/// One run of ks-play: its process, and where its output goes.
class PlayRun {
public:
    /// Starts ks-play with `args` in `cwd`, its output in `scratch`.
    PlayRun(const std::filesystem::path& scratch, const std::filesystem::path& cwd,
            const std::vector<std::string>& args) {
        static int counter = 0;
        const std::string tag = std::to_string(::getpid()) + "-" + std::to_string(counter++);
        out_ = scratch / ("play-" + tag + ".out");
        err_ = scratch / ("play-" + tag + ".err");
        std::vector<std::string> shell{
            "-c",          R"(err=$1; cwd=$2; shift 2; cd "$cwd" && exec "$@" >"$0" 2>"$err")",
            out_.string(), err_.string(),
            cwd.string(),  std::string{kPlayPath}};
        shell.insert(shell.end(), args.begin(), args.end());
        process_ = processes::test::start("/bin/sh", shell);
    }

    /// Waits up to `timeout` for ks-play to exit, and returns its status, or
    /// nothing if it's still running.
    std::optional<int> wait(Milliseconds timeout = Milliseconds{15'000}) {
        return process_.wait(timeout);
    }

    void signal(int signal) const noexcept { process_.signal(signal); }
    [[nodiscard]] bool running() { return process_.running(); }
    [[nodiscard]] std::string out() const { return slurp(out_); }
    [[nodiscard]] std::string err() const { return slurp(err_); }

private:
    Process process_;
    std::filesystem::path out_;
    std::filesystem::path err_;
};

/// What a finished run printed.
struct Finished {
    std::optional<int> status;
    std::string out;
    std::string err;
};

/// Runs ks-play with `--runtime-dir <dir>` and `args`, and waits for it.
inline Finished run_play(const std::filesystem::path& dir, std::vector<std::string> args,
                         const std::filesystem::path& cwd = "/") {
    args.insert(args.begin(), {"--runtime-dir", dir.string()});
    PlayRun run{dir, cwd, args};
    Finished finished;
    finished.status = run.wait();
    finished.out = run.out();
    finished.err = run.err();
    return finished;
}

/// Runs ks-play with exactly `args`.
inline Finished run_play_raw(const std::filesystem::path& scratch,
                             const std::vector<std::string>& args) {
    PlayRun run{scratch, "/", args};
    Finished finished;
    finished.status = run.wait();
    finished.out = run.out();
    finished.err = run.err();
    return finished;
}

/// The lines of `text`, without their newlines.
inline std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in{text};
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

/// A status line: its label (`engine`, `deck 0`, ...), and its fields in
/// order.
struct StatusLine {
    std::string label;
    std::vector<std::pair<std::string, std::string>> fields;

    [[nodiscard]] std::optional<std::string> get(std::string_view name) const {
        for (const auto& [key, value] : fields) {
            if (key == name) {
                return value;
            }
        }
        return std::nullopt;
    }
};

/// Splits `line` into its label and `name=value` fields. A label is the
/// words before the first field: `engine`, or `deck 0`.
inline StatusLine parse_status_line(const std::string& line) {
    StatusLine parsed;
    std::istringstream in{line};
    std::string word;
    std::string label;
    while (in >> word) {
        const std::size_t eq = word.find('=');
        if (eq == std::string::npos) {
            label += (label.empty() ? "" : " ") + word;
        } else {
            parsed.fields.emplace_back(word.substr(0, eq), word.substr(eq + 1));
        }
    }
    parsed.label = label;
    return parsed;
}

/// The status lines in `text`: the lines that aren't `---`.
inline std::vector<StatusLine> status_lines(const std::string& text) {
    std::vector<StatusLine> lines;
    for (const std::string& line : lines_of(text)) {
        if (line != "---" && !line.empty()) {
            lines.push_back(parse_status_line(line));
        }
    }
    return lines;
}

/// The line labeled `label`, or nothing.
inline std::optional<StatusLine> find_label(const std::vector<StatusLine>& lines,
                                            std::string_view label) {
    for (const StatusLine& line : lines) {
        if (line.label == label) {
            return line;
        }
    }
    return std::nullopt;
}

/// The field `name` of the line `label` in the output of a fresh
/// `ks-play status`, or nothing.
inline std::optional<std::string> status_field(const std::filesystem::path& dir,
                                               std::string_view label, std::string_view name) {
    const Finished status = run_play(dir, {"status"});
    if (status.status != 0) {
        return std::nullopt;
    }
    const std::optional<StatusLine> line = find_label(status_lines(status.out), label);
    return line ? line->get(name) : std::nullopt;
}

} // namespace ks::ks_play::test
