// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Helpers for the state-folder acceptance tests (specs/state-folder/spec.md)
// that run the programs: folders whose engines are killed with them, kool_spin
// with a state folder, and what a program prints. Built on the
// supervisor-crash tests' helpers.

#include <ks/ipc/messages.h>
#include <ks/ipc/runtime_dir.h>

#include "relaunch_support.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <csignal>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ks::state_folder::test {

using namespace ks::supervisor_crash::test;
using ipc::kEngineLockName;
using ipc::kSupervisorLockName;
using processes::test::lock_holder;

/// A folder of the test's own, for a runtime directory or a state folder.
/// Whatever still holds its engine lock at the end is killed, so declare it
/// before what runs in it.
class Folder {
public:
    Folder() { REQUIRE(!dir_.path().empty()); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return dir_.path(); }

private:
    ipc::test::TempDir dir_;
    processes::test::EngineReaper reaper_{dir_.path()};
};

/// Creates the folder `path` with mode 0700, whatever the umask: a state
/// folder that the group can write is refused.
inline void make_private(const std::filesystem::path& path) {
    REQUIRE(::mkdir(path.c_str(), 0700) == 0);
}

/// Sets a file's permissions, and puts them back when it's destroyed, so that
/// the folder can be removed.
class Mode {
public:
    Mode(std::filesystem::path path, mode_t mode) : path_{std::move(path)} {
        struct stat status{};
        REQUIRE(::stat(path_.c_str(), &status) == 0);
        before_ = static_cast<mode_t>(status.st_mode & 07777U);
        REQUIRE(::chmod(path_.c_str(), mode) == 0);
    }
    Mode(const Mode&) = delete;
    Mode& operator=(const Mode&) = delete;
    Mode(Mode&&) = delete;
    Mode& operator=(Mode&&) = delete;
    ~Mode() { static_cast<void>(::chmod(path_.c_str(), before_)); }

private:
    std::filesystem::path path_;
    mode_t before_ = 0;
};

/// Permissions don't stop root.
inline void skip_as_root() {
    if (::geteuid() == 0) {
        SKIP("root may write anything");
    }
}

inline std::string process_text(pid_t pid) {
    return "(process " + std::to_string(pid) + ")";
}

inline bool has(const std::string& text, std::string_view phrase) {
    return text.find(phrase) != std::string::npos;
}

/// kool_spin's options for the state folder `state`.
inline std::vector<std::string> with_state(const std::filesystem::path& state,
                                           std::vector<std::string> more = {}) {
    more.insert(more.begin(), {"--state-dir", state.string()});
    return more;
}

inline engine_restart::test::SessionOptions session_options(const std::filesystem::path& state) {
    engine_restart::test::SessionOptions options;
    options.supervisor = with_state(state);
    return options;
}

/// kool_spin with the state folder `state`, playing a constant from `files` on
/// deck 0, with a recording.
inline std::unique_ptr<Playing> playing_with(const Files& files,
                                             const std::filesystem::path& state) {
    auto session = engine_restart::test::start_playing(
        engine_restart::test::write_constant(files.path(), "constant.wav", 120),
        session_options(state));
    session->play(0);
    REQUIRE(playing_deck(session->client.state, 0, 0.5 * kRate).has_value());
    return session;
}

/// The same, then killed with SIGKILL: its engine is an orphan.
inline std::unique_ptr<Playing> orphaned_with(const Files& files,
                                              const std::filesystem::path& state) {
    auto session = playing_with(files, state);
    session->session.process().signal(SIGKILL);
    REQUIRE(session->session.process().wait() == 128 + SIGKILL);
    REQUIRE(session->session.engine() > 0);
    return session;
}

/// Stops `dir`'s kool_spin through a client.
inline bool stop(const std::filesystem::path& dir) {
    std::expected<engine_restart::test::Client, std::string> client =
        engine_restart::test::join(dir, "stopper");
    return client && engine_restart::test::ask(*client, ipc::msg::Stop{}).has_value();
}

/// True if a client of `dir`'s kool_spin gets an answer.
inline bool answers(const std::filesystem::path& dir) {
    std::expected<engine_restart::test::Client, std::string> client =
        engine_restart::test::join(dir, "looker");
    return client && processes::test::pings(client->connection, 2);
}

/// True if `dir`'s lock `name` is held by process `pid` within 5 s.
inline bool held_by(const std::filesystem::path& dir, std::string_view name, pid_t pid) {
    return pid > 0 && eventually([&] { return lock_holder(dir, name) == pid; });
}

/// True if neither the supervisor nor the engine lock in `dir` is held.
inline bool all_free(const std::filesystem::path& dir) {
    return !lock_holder(dir, kSupervisorLockName) && !lock_holder(dir, kEngineLockName);
}

/// Everything in `dir`.
inline std::set<std::filesystem::path> files_in(const std::filesystem::path& dir) {
    std::set<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator{dir}) {
        files.insert(entry.path());
    }
    return files;
}

/// A program running with its standard output and standard error in a file,
/// and no terminal. `env` is /usr/bin/env's arguments, such as "HOME=/x" and
/// "-u NAME", so the test never changes its own environment.
class Running {
public:
    Running(const std::filesystem::path& scratch, std::string_view program,
            const std::vector<std::string>& args, const std::vector<std::string>& env = {}) {
        static int counter = 0;
        out_ = scratch / ("running-" + std::to_string(counter++) + ".out");
        std::vector<std::string> shell{"-c", R"(exec /usr/bin/env "$@" </dev/null >"$0" 2>&1)",
                                       out_.string()};
        shell.insert(shell.end(), env.begin(), env.end());
        shell.emplace_back(program);
        shell.insert(shell.end(), args.begin(), args.end());
        process_ = processes::test::start("/bin/sh", shell);
        REQUIRE(process_.pid() > 0);
    }

    [[nodiscard]] Process& process() noexcept { return process_; }
    [[nodiscard]] std::string out() const {
        std::ifstream in{out_, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }

private:
    std::filesystem::path out_;
    Process process_;
};

/// A program that has run: its exit status, and what it printed.
struct Finished {
    std::optional<int> status;
    std::string out;
};

inline Finished finished(const std::filesystem::path& scratch, std::string_view program,
                         const std::vector<std::string>& args) {
    Running running{scratch, program, args};
    Finished result;
    result.status = running.process().wait(kLaunchWait);
    result.out = running.out();
    return result;
}

} // namespace ks::state_folder::test
