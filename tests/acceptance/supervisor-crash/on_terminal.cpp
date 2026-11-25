// SPDX-License-Identifier: GPL-3.0-or-later
// ks_on_terminal: a helper for the supervisor-crash acceptance tests.
//
//   ks_on_terminal <tty> foreground|background <program> [<arg>...]
//
// Starts a session whose controlling terminal is <tty>, and runs <program> in
// it, with standard input, output, and error on the terminal: in the
// terminal's foreground process group, as the session's leader, or in a
// process group in the background. Exits with the program's status, or 128
// plus the signal that killed it.

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <span>
#include <string_view>

namespace {

/// Waits for `pid` and returns its status as a shell reports it.
int wait_for(pid_t pid) {
    int status = 0;
    while (::waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            return 125;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/// In the forked child: never returns.
[[noreturn]] void in_session(const char* tty, bool background, char** program) {
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg): open() and ioctl() are variadic
    const int fd = ::setsid() == -1 ? -1 : ::open(tty, O_RDWR);
    // On Linux, opening it has made it the controlling terminal already.
    if (fd == -1 || (::ioctl(fd, TIOCSCTTY, 0) == -1 && ::tcgetsid(fd) != ::getpid())) {
        ::_exit(126);
    }
    // NOLINTEND(cppcoreguidelines-pro-type-vararg)
    for (const int standard : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
        if (::dup2(fd, standard) == -1) {
            ::_exit(126);
        }
    }
    if (fd > STDERR_FILENO) {
        ::close(fd);
    }
    if (background) {
        // This process is the session's shell, and the program its
        // background job.
        const pid_t job = ::fork();
        if (job != 0) {
            ::_exit(job == -1 ? 126 : wait_for(job));
        }
        if (::setpgid(0, 0) == -1) {
            ::_exit(126);
        }
    }
    ::execv(*program, program);
    ::_exit(127);
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    if (args.size() < 4) {
        return 125;
    }
    // A process group leader, which this is, can't start a session.
    const pid_t child = ::fork();
    if (child == 0) {
        in_session(args[1], std::string_view{args[2]} == "background", args.subspan(3).data());
    }
    return child == -1 ? 125 : wait_for(child);
}
