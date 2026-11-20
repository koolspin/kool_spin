// SPDX-License-Identifier: GPL-3.0-or-later
// ks_process_probe: a helper for the processes acceptance tests. It reports
// what it inherited, and can stand in for ks-engine, misbehaving on purpose.
//
//   --report <path>        write a report to <path> (via a temporary file and
//                          a rename, so it appears whole): "pid", "pgid", one
//                          "fd <n> <device> <inode>" per open descriptor,
//                          one "blocked <signal>" per blocked signal,
//                          "disposition <INT|TERM|HUP|PIPE> <default|ignored|handled>",
//                          one "arg <hex>" per argument ("-" if empty), and
//                          "end"
//   --mode <mode>          exit (the default): exit with status 0
//                          silent: never exit, ignoring SIGTERM, SIGINT, and
//                            SIGHUP
//                          ready-stuck: send Ready on --supervisor-fd, then
//                            as silent, never reading
//                          crash: send Ready, then abort(); but given
//                            --clean, as ready-stuck (specs/engine-restart/
//                            spec.md requirement 11)
//                          crash-always: send Ready, then abort(), whatever
//                            else it's given
//                          crash-then-silent: the first time it runs in a
//                            runtime directory (it leaves a file there to
//                            say so), as crash. Every later time, without
//                            --clean, as silent: it never sends Ready. With
//                            --clean, as ready-stuck.
//   --supervisor-fd <n>    the socket ks-engine would get from kool_spin
//   --runtime-dir <path>   if given, the probe takes the engine lock there,
//                          as ks-engine would
//
// Anything else is ignored, so the probe accepts ks-engine's arguments. Its
// descriptors are listed before it opens any of its own. In silent modes it
// exits after 60 s anyway, so nothing a failed test leaves runs for long.

#include <ks/ipc/connection.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/frame.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/runtime_dir.h>

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

/// The highest descriptor the report looks at.
constexpr int kMaxFd = 1024;

/// The probe's command line.
struct Arguments {
    std::optional<std::string> report;
    std::string mode = "exit";
    std::optional<int> supervisor_fd;
    std::optional<std::string> runtime_dir;
};

Arguments parse(std::span<const std::string> args) {
    Arguments result;
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        const std::string& arg = args[i];
        const std::string& value = args[i + 1];
        if (arg == "--report") {
            result.report = value;
        } else if (arg == "--mode") {
            result.mode = value;
        } else if (arg == "--supervisor-fd") {
            int fd = -1;
            const char* const first = std::to_address(value.begin());
            const char* const last = std::to_address(value.end());
            const auto [end, error] = std::from_chars(first, last, fd);
            if (error == std::errc{} && end == last) {
                result.supervisor_fd = fd;
            }
        } else if (arg == "--runtime-dir") {
            result.runtime_dir = value;
        } else {
            continue;
        }
        ++i;
    }
    return result;
}

/// "fd <n> <device> <inode>" for each open descriptor.
std::string descriptors() {
    std::string lines;
    for (int fd = 0; fd < kMaxFd; ++fd) {
        struct stat status{};
        if (::fstat(fd, &status) == 0) {
            lines +=
                std::format("fd {} {} {}\n", fd, static_cast<unsigned long long>(status.st_dev),
                            static_cast<unsigned long long>(status.st_ino));
        }
    }
    return lines;
}

/// "blocked <signal>" for each blocked signal.
std::string blocked_signals() {
    sigset_t mask;
    sigemptyset(&mask);
    static_cast<void>(::pthread_sigmask(SIG_BLOCK, nullptr, &mask));
    std::string lines;
    for (int signal = 1; signal < 32; ++signal) {
        if (sigismember(&mask, signal) == 1) {
            lines += std::format("blocked {}\n", signal);
        }
    }
    return lines;
}

/// "default", "ignored", or "handled".
std::string_view disposition(int signal) {
    struct sigaction action{};
    static_cast<void>(::sigaction(signal, nullptr, &action));
    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr):
    // sigaction's handler is a union member, and SIG_DFL and SIG_IGN are casts
    if (action.sa_handler == SIG_DFL) {
        return "default";
    }
    if (action.sa_handler == SIG_IGN) {
        return "ignored";
    }
    // NOLINTEND(cppcoreguidelines-pro-type-union-access,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
    return "handled";
}

std::string hex(std::string_view text) {
    if (text.empty()) {
        return "-";
    }
    std::string result;
    for (const char c : text) {
        result += std::format("{:02x}", static_cast<unsigned char>(c));
    }
    return result;
}

/// Writes the report to `path`, whole.
void write_report(const std::string& path, const std::string& fds,
                  std::span<const std::string> args) {
    std::string text = std::format("pid {}\npgid {}\n", ::getpid(), ::getpgrp());
    text += fds;
    text += blocked_signals();
    for (const auto& [number, name] : {std::pair{SIGINT, "INT"}, std::pair{SIGTERM, "TERM"},
                                       std::pair{SIGHUP, "HUP"}, std::pair{SIGPIPE, "PIPE"}}) {
        text += std::format("disposition {} {}\n", name, disposition(number));
    }
    for (const std::string& arg : args) {
        text += std::format("arg {}\n", hex(arg));
    }
    text += "end\n";

    const std::string temporary = path + ".tmp";
    {
        std::ofstream out{temporary};
        out << text;
    }
    std::error_code ignored;
    std::filesystem::rename(temporary, path, ignored);
}

extern "C" void ignore_signal(int /*signal*/) {}

/// Never returns: ignores SIGTERM, SIGINT, and SIGHUP, and exits after 60 s.
[[noreturn]] void stay() {
    for (const int signal : {SIGTERM, SIGINT, SIGHUP}) {
        static_cast<void>(std::signal(signal, &ignore_signal));
    }
    ::alarm(60);
    for (;;) {
        ::pause();
    }
}

} // namespace

namespace {

/// The probe's work, as main() describes it.
int probe(std::span<char*> arguments) {
    // Listed first, before the probe opens anything.
    const std::string fds = descriptors();

    std::vector<std::string> args;
    for (const char* argument : arguments.subspan(1)) {
        args.emplace_back(argument);
    }
    Arguments parsed = parse(args);
    if (parsed.report) {
        write_report(*parsed.report, fds, args);
    }

    std::optional<ks::ipc::LockFile> lock;
    if (parsed.runtime_dir) {
        std::expected<ks::ipc::LockFile, ks::ipc::LockFile::Failure> acquired =
            ks::ipc::LockFile::acquire(*parsed.runtime_dir, ks::ipc::kEngineLockName);
        if (!acquired) {
            return 1;
        }
        lock = std::move(*acquired);
    }

    if (parsed.mode == "silent") {
        stay();
    }
    const bool clean = std::ranges::find(args, "--clean") != args.end();
    if (parsed.mode == "crash-then-silent") {
        // The first run of the probe in this runtime directory crashes after
        // Ready; the later ones that aren't clean never send Ready.
        bool first = true;
        if (parsed.runtime_dir) {
            std::error_code ignored;
            first = std::filesystem::create_directory(
                std::filesystem::path{*parsed.runtime_dir} / "probe-started", ignored);
        }
        if (!first && !clean) {
            stay();
        }
        parsed.mode = first ? "crash" : "ready-stuck";
    }
    const bool crash = parsed.mode == "crash-always" || (parsed.mode == "crash" && !clean);
    if (parsed.mode == "ready-stuck" || parsed.mode == "crash" || parsed.mode == "crash-always") {
        if (!parsed.supervisor_fd) {
            return 1;
        }
        ks::ipc::Connection supervisor{ks::ipc::Fd{*parsed.supervisor_fd}};
        const std::expected<ks::ipc::Frame, ks::ipc::MessageError> ready =
            ks::ipc::encode(ks::ipc::Message{.request_id = 0, .body = ks::ipc::msg::Ready{}});
        if (!ready || !supervisor.send(*ready, {}) || !supervisor.flush()) {
            return 1;
        }
        if (crash) {
            std::abort();
        }
        stay();
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return probe(std::span<char*>{argv, static_cast<std::size_t>(argc)});
    } catch (...) {
        return 2;
    }
}
