// SPDX-License-Identifier: GPL-3.0-or-later
// For the health-check tests: sets resource limits, then executes a program,
// so that kool_spin, and the engine it starts, inherit them
// (specs/health-checks/spec.md, "Acceptance criteria").
//
//   ks_rlimit_exec [rtprio <soft> <hard>] [memlock <soft> <hard>] -- <program> [args...]
//
// Each limit is a whole number, `unlimited`, or `keep` (as it is now). On a
// failure it writes one line to standard error and exits with status 127.

#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace {

[[noreturn]] void fail(const char* what) {
    std::fputs("ks_rlimit_exec: ", stderr);
    std::fputs(what, stderr);
    std::fputs("\n", stderr);
    std::_Exit(127);
}

std::optional<rlim_t> limit_of(std::string_view text, rlim_t now) {
    if (text == "keep") {
        return now;
    }
    if (text == "unlimited") {
        return RLIM_INFINITY;
    }
    unsigned long long value = 0;
    const char* const first = std::to_address(text.begin());
    const char* const last = std::to_address(text.end());
    const auto [end, error] = std::from_chars(first, last, value);
    if (error != std::errc{} || end != last) {
        return std::nullopt;
    }
    return static_cast<rlim_t>(value);
}

void set(int resource, std::string_view soft, std::string_view hard) {
    rlimit current{};
    if (::getrlimit(resource, &current) != 0) {
        fail("getrlimit failed");
    }
    const std::optional<rlim_t> soft_value = limit_of(soft, current.rlim_cur);
    const std::optional<rlim_t> hard_value = limit_of(hard, current.rlim_max);
    if (!soft_value || !hard_value) {
        fail("a limit must be a whole number, unlimited, or keep");
    }
    const rlimit wanted{.rlim_cur = *soft_value, .rlim_max = *hard_value};
    if (::setrlimit(resource, &wanted) != 0) {
        fail(std::strerror(errno)); // NOLINT(concurrency-mt-unsafe): single-threaded
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    std::size_t i = 1;
    while (i < args.size() && std::string_view{args[i]} != "--") {
        if (i + 2 >= args.size()) {
            fail("a limit needs a soft and a hard value");
        }
        const std::string_view name{args[i]};
#ifdef RLIMIT_RTPRIO
        if (name == "rtprio") {
            set(RLIMIT_RTPRIO, args[i + 1], args[i + 2]);
            i += 3;
            continue;
        }
#endif
        if (name == "memlock") {
            set(RLIMIT_MEMLOCK, args[i + 1], args[i + 2]);
            i += 3;
            continue;
        }
        fail("unknown limit");
    }
    if (i + 1 >= args.size()) {
        fail("no program after --");
    }
    std::vector<char*> program{args.begin() + static_cast<std::ptrdiff_t>(i + 1), args.end()};
    program.push_back(nullptr);
    ::execv(program.front(), program.data());
    fail(std::strerror(errno)); // NOLINT(concurrency-mt-unsafe): single-threaded
}
