// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/play_app/run.h>

#include <ks/play_app/options.h>

#include <catch2/catch_test_macros.hpp>
#include <pthread.h>

#include <csignal>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Ran {
    int status = -1;
    std::string out;
    std::string err;
};

Ran run_main(std::vector<std::string> args) {
    args.insert(args.begin(), "ks-play");
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (const std::string& arg : args) {
        argv.push_back(arg.c_str());
    }
    std::ostringstream out;
    std::ostringstream err;
    Ran ran;
    ran.status = ks::play_app::main_entry(static_cast<int>(argv.size()), argv.data(), out, err);
    ran.out = out.str();
    ran.err = err.str();
    return ran;
}

// A runtime directory that can't exist: there's no supervisor socket in it.
const std::string kMissingDir = "/nonexistent/ks-play-run-test";

} // namespace

TEST_CASE("ks-play main_entry: usage errors", "[play_app]") {
    const std::vector<std::vector<std::string>> bad{
        {},
        {"frobnicate"},
        {"ping", "now"},
        {"play", "4"},
        {"rate", "0", "nan"},
        {"--name", "", "ping"},
        {"--runtime-dir", kMissingDir, "load", "0", ""},
    };
    for (const std::vector<std::string>& args : bad) {
        CAPTURE(args);
        const Ran ran = run_main(args);
        CHECK(ran.status == ks::play_app::kExitUsage);
        CHECK(ran.out.empty());
        CHECK(ran.err.starts_with("ks-play: "));
        CHECK(ran.err.ends_with(" (see ks-play --help)\n"));
        CHECK(ran.err.find('\n') == ran.err.size() - 1);
    }
}

TEST_CASE("ks-play main_entry: --help", "[play_app]") {
    const Ran ran = run_main({"--help"});
    CHECK(ran.status == ks::play_app::kExitOk);
    CHECK(ran.out == ks::play_app::usage());
    CHECK(ran.err.empty());
}

TEST_CASE("ks-play main_entry: no kool_spin", "[play_app]") {
    const std::vector<std::vector<std::string>> commands{
        {"ping"},
        {"stop"},
        {"unload", "1"},
        {"status"},
        {"play", "0"},
        {"load", "0", "/a.flac", "--wait"},
        {"load", "0", "relative.flac"},
    };
    for (std::vector<std::string> args : commands) {
        CAPTURE(args);
        args.insert(args.begin(), {"--runtime-dir", kMissingDir});
        const Ran ran = run_main(args);
        CHECK(ran.status == ks::play_app::kExitUnreachable);
        CHECK(ran.out.empty());
        CHECK(ran.err.starts_with("ks-play: can't reach kool_spin in " + kMissingDir + ": "));
        CHECK(ran.err.find('\n') == ran.err.size() - 1);
    }
}

TEST_CASE("ks-play run: a relative path is refused before connecting", "[play_app]") {
    const ks::play_app::Invocation invocation{
        .runtime_dir = kMissingDir,
        .name = "ks-play",
        .request = ks::play_app::Load{.deck = 0, .path = "relative.flac", .wait = false},
    };
    std::ostringstream out;
    std::ostringstream err;
    CHECK(ks::play_app::run(invocation, out, err) == ks::play_app::kExitFailed);
    CHECK(out.str().empty());
    CHECK(err.str().starts_with("ks-play: "));
}

TEST_CASE("ks-play run: signals are blocked only while it runs", "[play_app]") {
    sigset_t before{};
    REQUIRE(pthread_sigmask(SIG_SETMASK, nullptr, &before) == 0);
    const auto pipe_before = std::signal(SIGPIPE, SIG_DFL);
    std::signal(SIGPIPE, pipe_before);

    const std::vector<ks::play_app::Request> requests{
        ks::play_app::Ping{},
        ks::play_app::Status{.watch = false, .interval_ms = 500},
        ks::play_app::Status{.watch = true, .interval_ms = 50},
        ks::play_app::Load{.deck = 0, .path = "/a.flac", .wait = true},
    };
    for (const ks::play_app::Request& request : requests) {
        const ks::play_app::Invocation invocation{
            .runtime_dir = kMissingDir,
            .name = "ks-play",
            .request = request,
        };
        std::ostringstream out;
        std::ostringstream err;
        CHECK(ks::play_app::run(invocation, out, err) == ks::play_app::kExitUnreachable);
        CHECK(out.str().empty());
        CHECK(err.str().starts_with("ks-play: can't reach kool_spin in " + kMissingDir + ": "));

        // The mask is back as it was, and the worker's wake-up isn't left
        // pending (it would end this process once unblocked).
        sigset_t after{};
        REQUIRE(pthread_sigmask(SIG_SETMASK, nullptr, &after) == 0);
        CHECK(sigismember(&after, SIGINT) == sigismember(&before, SIGINT));
        CHECK(sigismember(&after, SIGTERM) == sigismember(&before, SIGTERM));
        CHECK(sigismember(&after, SIGUSR1) == sigismember(&before, SIGUSR1));
        sigset_t pending{};
        REQUIRE(sigpending(&pending) == 0);
        CHECK(sigismember(&pending, SIGINT) == 0);
        CHECK(sigismember(&pending, SIGTERM) == 0);
        CHECK(sigismember(&pending, SIGUSR1) == 0);

        // SIGPIPE's disposition is put back too.
        const auto pipe_after = std::signal(SIGPIPE, SIG_DFL);
        std::signal(SIGPIPE, pipe_after);
        CHECK(pipe_after == pipe_before);
    }
}

TEST_CASE("ks-play run: a failed write to standard output", "[play_app]") {
    const ks::play_app::Invocation invocation{
        .runtime_dir = kMissingDir,
        .name = "ks-play",
        .request = ks::play_app::Help{},
    };
    std::ostringstream out;
    out.setstate(std::ios::badbit);
    std::ostringstream err;
    CHECK(ks::play_app::run(invocation, out, err) == ks::play_app::kExitFailed);
    CHECK(err.str().starts_with("ks-play: "));
}
