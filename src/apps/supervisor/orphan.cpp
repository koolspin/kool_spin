// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/supervisor/orphan.h>

#include <ks/ipc/fd.h>
#include <ks/ipc/runtime_dir.h>
#include <ks/supervisor/run.h>

#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace ks::supervisor {

namespace {

using Milliseconds = std::chrono::milliseconds;

std::expected<ipc::LockState, std::error_code> probe(const std::filesystem::path& dir) {
    return ipc::probe_lock(dir, ipc::kEngineLockName);
}

std::string probe_failed(std::error_code error) {
    return std::format("can't check the engine lock: {}", error.message());
}

/// A pidfd for `pid`, or an invalid Fd if there's no such process, or no such
/// thing as a pidfd.
ipc::Fd open_pidfd([[maybe_unused]] pid_t pid) noexcept {
#ifdef __linux__
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): syscall() is variadic
    return ipc::Fd{static_cast<int>(::syscall(SYS_pidfd_open, pid, 0))};
#else
    return ipc::Fd{};
#endif
}

/// Sends `signal` to the process `pidfd` refers to, or to `pid` if it's
/// invalid. Returns 0, or errno.
int send_signal([[maybe_unused]] const ipc::Fd& pidfd, pid_t pid, int signal) noexcept {
#ifdef __linux__
    if (pidfd.valid()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): syscall() is variadic
        if (::syscall(SYS_pidfd_send_signal, pidfd.get(), signal, nullptr, 0) == 0) {
            return 0;
        }
        // A kernel, or a sandbox, that opens pidfds but doesn't signal them.
        if (errno != ENOSYS) {
            return errno;
        }
    }
#endif
    return ::kill(pid, signal) == 0 ? 0 : errno;
}

/// Sends `signal` to the orphan, if it still holds the lock (requirement 4).
/// False if the lock is free. Says why it couldn't.
std::expected<bool, std::string> signal_holder(const std::filesystem::path& dir, pid_t pid,
                                               int signal) {
    // Opened before the probe: if the probe finds the lock held by `pid`, the
    // pidfd either refers to the holder, or to a process that had the ID
    // before it and has ended, and signaling it does nothing.
    const ipc::Fd pidfd = open_pidfd(pid);
    const std::expected<ipc::LockState, std::error_code> state = probe(dir);
    if (!state) {
        return std::unexpected{probe_failed(state.error())};
    }
    if (!state->held) {
        return false;
    }
    if (state->pid != pid) {
        return std::unexpected{std::string{"another process has taken the engine lock"}};
    }
    if (const int error = send_signal(pidfd, pid, signal); error != 0) {
        if (error == ESRCH) {
            // It may be ending right now: the lock will tell.
            return true;
        }
        return std::unexpected{
            std::format("can't signal process {}: {}", pid,
                        std::error_code{error, std::system_category()}.message())};
    }
    return true;
}

/// Waits up to `limit` for the engine lock to come free. True if it did.
std::expected<bool, std::string> wait_free(const std::filesystem::path& dir, Milliseconds limit,
                                           const Wait& wait) {
    for (Milliseconds waited{0};; waited += kOrphanPoll) {
        const std::expected<ipc::LockState, std::error_code> state = probe(dir);
        if (!state) {
            return std::unexpected{probe_failed(state.error())};
        }
        if (!state->held) {
            return true;
        }
        if (waited >= limit) {
            return false;
        }
        wait(kOrphanPoll);
    }
}

bool is_blank(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r';
}

char lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

std::expected<std::optional<Orphan>, std::error_code> find_orphan(const std::filesystem::path& dir,
                                                                  const Wait& wait) {
    std::optional<pid_t> last;
    for (Milliseconds waited{0};; waited += kOrphanSettle) {
        const std::expected<ipc::LockState, std::error_code> state = probe(dir);
        if (!state) {
            return std::unexpected{state.error()};
        }
        if (!state->held) {
            return std::nullopt;
        }
        // Its own ID is an earlier process's: this one holds no engine lock.
        const std::optional<pid_t> now =
            state->pid == ::getpid() ? std::optional<pid_t>{} : state->pid;
        if (now && now == last) {
            return Orphan{.pid = now};
        }
        if (waited >= kOrphanSettleLimit) {
            return Orphan{};
        }
        last = now;
        wait(kOrphanSettle);
    }
}

std::expected<Stopped, std::string> stop_orphan(const std::filesystem::path& dir, pid_t pid,
                                                const Wait& wait) {
    struct Step {
        int signal;
        Milliseconds limit;
        Stopped ended;
    };
    const std::array<Step, 2> steps{
        Step{.signal = SIGTERM, .limit = kEngineStopTimeout, .ended = Stopped::Faded},
        Step{.signal = SIGKILL, .limit = kOrphanKillTimeout, .ended = Stopped::Killed}};
    std::optional<Stopped> sent;
    for (const Step& step : steps) {
        const std::expected<bool, std::string> signaled = signal_holder(dir, pid, step.signal);
        if (!signaled) {
            return std::unexpected{signaled.error()};
        }
        if (!*signaled) {
            return sent.value_or(Stopped::Gone);
        }
        sent = step.ended;
        const std::expected<bool, std::string> free = wait_free(dir, step.limit, wait);
        if (!free) {
            return std::unexpected{free.error()};
        }
        if (*free) {
            return step.ended;
        }
    }
    return std::unexpected{std::format("process {} didn't end, even when killed", pid)};
}

bool can_ask(int in, int err) noexcept {
    return ::isatty(in) == 1 && ::isatty(err) == 1 && ::tcgetpgrp(in) == ::getpgrp();
}

bool is_yes(std::string_view answer) noexcept {
    while (!answer.empty() && is_blank(answer.front())) {
        answer.remove_prefix(1);
    }
    while (!answer.empty() && is_blank(answer.back())) {
        answer.remove_suffix(1);
    }
    const auto is = [answer](std::string_view word) {
        return std::ranges::equal(answer, word, {}, lower);
    };
    return is("y") || is("yes");
}

bool read_answer(int in) noexcept {
    std::array<char, kMaxAnswer> line{};
    std::size_t size = 0;
    bool fits = true;
    for (;;) {
        char c = 0;
        const ssize_t got = ::read(in, &c, 1);
        if (got == -1 && errno == EINTR) {
            continue;
        }
        if (got != 1) {
            return false;
        }
        if (c == '\n') {
            return fits && is_yes(std::string_view{line.data(), size});
        }
        fits = fits && size < line.size();
        if (fits) {
            std::span{line}[size] = c;
            ++size;
        }
    }
}

} // namespace ks::supervisor
