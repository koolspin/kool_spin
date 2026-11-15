// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/processes/spec.md: starting children
// (requirement 17).

#include <ks/supervisor/spawn.h>

#include <ks/ipc/fd.h>

#include "processes_support.h"
#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace {

using ks::ipc::Fd;
using ks::ipc::test::file_id;
using ks::ipc::test::FileId;
using ks::ipc::test::TempDir;
using ks::processes::test::kProbePath;
using ks::processes::test::ProbeReport;
using ks::processes::test::Process;
using ks::processes::test::start;
using ks::processes::test::wait_for_report;
using ks::supervisor::Inherit;
using ks::supervisor::spawn;

/// The descriptors among 0, 1, and 2 that this process has open: a child
/// inherits these.
std::set<int> open_standard_descriptors() {
    std::set<int> fds;
    for (int fd = 0; fd <= 2; ++fd) {
        struct stat status{};
        if (::fstat(fd, &status) == 0) {
            fds.insert(fd);
        }
    }
    return fds;
}

/// The descriptors in `report`.
std::set<int> descriptors_of(const ProbeReport& report) {
    std::set<int> fds;
    for (const auto& [fd, id] : report.fds) {
        fds.insert(fd);
    }
    return fds;
}

/// Puts back the signal mask and the dispositions of SIGINT and SIGPIPE.
class SignalState {
public:
    SignalState() {
        static_cast<void>(::pthread_sigmask(SIG_SETMASK, nullptr, &mask_));
        static_cast<void>(::sigaction(SIGINT, nullptr, &interrupt_));
        static_cast<void>(::sigaction(SIGPIPE, nullptr, &pipe_));
    }
    SignalState(const SignalState&) = delete;
    SignalState& operator=(const SignalState&) = delete;
    SignalState(SignalState&&) = delete;
    SignalState& operator=(SignalState&&) = delete;

    ~SignalState() {
        static_cast<void>(::sigaction(SIGINT, &interrupt_, nullptr));
        static_cast<void>(::sigaction(SIGPIPE, &pipe_, nullptr));
        static_cast<void>(::pthread_sigmask(SIG_SETMASK, &mask_, nullptr));
    }

private:
    sigset_t mask_{};
    struct sigaction interrupt_{};
    struct sigaction pipe_{};
};

/// Ignores `signal`, as a parent might before starting a child.
void ignore(int signal) {
    struct sigaction action{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr):
    // sigaction's handler is a union member, and SIG_IGN is a cast
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    static_cast<void>(::sigaction(signal, &action, nullptr));
}

} // namespace

TEST_CASE("Spawning: only the given descriptors", "[spawn]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());
    std::vector<Fd> files = ks::ipc::test::open_files(dir.path(), 2);
    REQUIRE(files.size() == 2);

    // Descriptors without close-on-exec: the child mustn't inherit them. One
    // has the next free number, below the child numbers; the other is above
    // them all.
    const std::string leaked_path = (dir.path() / "leaked").string();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open() is variadic
    const Fd leaked{::open(leaked_path.c_str(), O_RDONLY | O_CREAT | O_EXCL, 0600)};
    REQUIRE(leaked.valid());
    REQUIRE(::dup2(leaked.get(), 60) == 60);
    const Fd leaked_high{60};

    const std::set<int> standard = open_standard_descriptors();

    SECTION("at 3 and 5") {
        const std::filesystem::path report = dir.path() / "report";
        const std::vector<Inherit> fds{{.parent = files.at(0).get(), .child = 3},
                                       {.parent = files.at(1).get(), .child = 5}};
        Process probe = start(kProbePath, {"--report", report.string()}, fds);
        REQUIRE(probe.pid() > 0);
        REQUIRE(probe.wait() == 0);

        const std::expected<ProbeReport, std::string> found = wait_for_report(report);
        REQUIRE(found.has_value());
        std::set<int> expected = standard;
        expected.insert(3);
        expected.insert(5);
        CHECK(descriptors_of(*found) == expected);
        CHECK(found->fds.count(3) == 1);
        CHECK(found->fds.count(5) == 1);
        CHECK(std::optional<FileId>{found->fds.at(3)} == file_id(files.at(0).get()));
        CHECK(std::optional<FileId>{found->fds.at(5)} == file_id(files.at(1).get()));
    }

    SECTION("swapped") {
        // Each source sits at the other's target number, so the descriptors
        // must be moved without one overwriting the other.
        REQUIRE(::dup2(files.at(0).get(), 40) == 40);
        const Fd first{40};
        REQUIRE(::dup2(files.at(1).get(), 41) == 41);
        const Fd second{41};
        const std::filesystem::path report = dir.path() / "report";
        const std::vector<Inherit> fds{{.parent = 40, .child = 41}, {.parent = 41, .child = 40}};
        Process probe = start(kProbePath, {"--report", report.string()}, fds);
        REQUIRE(probe.pid() > 0);
        REQUIRE(probe.wait() == 0);

        const std::expected<ProbeReport, std::string> found = wait_for_report(report);
        REQUIRE(found.has_value());
        std::set<int> expected = standard;
        expected.insert(40);
        expected.insert(41);
        CHECK(descriptors_of(*found) == expected);
        REQUIRE(found->fds.count(40) == 1);
        REQUIRE(found->fds.count(41) == 1);
        CHECK(std::optional<FileId>{found->fds.at(41)} == file_id(files.at(0).get()));
        CHECK(std::optional<FileId>{found->fds.at(40)} == file_id(files.at(1).get()));
    }
}

TEST_CASE("Spawning: a new process group and default signals", "[spawn]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());
    const std::filesystem::path report = dir.path() / "report";
    const std::vector<std::string> args{"--report", report.string(), "", "two words", "last"};

    // Started with SIGUSR1 blocked, and SIGINT and SIGPIPE ignored, all put
    // back afterwards.
    Process probe = [&] {
        const SignalState restore;
        sigset_t block;
        sigemptyset(&block);
        sigaddset(&block, SIGUSR1);
        static_cast<void>(::pthread_sigmask(SIG_BLOCK, &block, nullptr));
        ignore(SIGINT);
        ignore(SIGPIPE);
        return start(kProbePath, args);
    }();
    REQUIRE(probe.pid() > 0);
    REQUIRE(probe.wait() == 0);

    const std::expected<ProbeReport, std::string> found = wait_for_report(report);
    REQUIRE(found.has_value());
    CHECK(found->pid == probe.pid());
    CHECK(found->pgid == found->pid);
    CHECK(found->pgid != ::getpgrp());
    CHECK(found->blocked.empty());
    const std::map<std::string, std::string> defaults{
        {"INT", "default"}, {"TERM", "default"}, {"HUP", "default"}, {"PIPE", "default"}};
    CHECK(found->dispositions == defaults);
    CHECK(found->args == args);
}

TEST_CASE("Spawning: a program that can't start", "[spawn]") {
    const TempDir dir;
    REQUIRE(!dir.path().empty());

    // A program that can start does.
    {
        Process probe = start(kProbePath, {});
        REQUIRE(probe.pid() > 0);
        REQUIRE(probe.wait() == 0);
    }

    const std::expected<pid_t, std::error_code> missing = spawn(dir.path() / "missing", {}, {});
    CHECK_FALSE(missing.has_value());

    const std::filesystem::path script = dir.path() / "not-executable";
    {
        std::ofstream out{script};
        out << "#!/bin/sh\nexit 0\n";
    }
    REQUIRE(::chmod(script.c_str(), 0644) == 0);
    const std::expected<pid_t, std::error_code> not_executable = spawn(script, {}, {});
    CHECK_FALSE(not_executable.has_value());

    // No child was left behind, not even a zombie.
    int status = 0;
    CHECK(::waitpid(-1, &status, WNOHANG) == -1);
    CHECK(errno == ECHILD);
}
