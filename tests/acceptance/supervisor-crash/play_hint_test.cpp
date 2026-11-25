// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/supervisor-crash/spec.md: what ks-play says when
// an engine runs without kool_spin (requirement 18).

#include <ks/ipc/runtime_dir.h>

#include "relaunch_support.h"
#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace ks::supervisor_crash::test;
using ks::ipc::LockFile;

constexpr std::string_view kHint = "an engine is running without kool_spin";

/// What `ks-play status` in `dir` did: its status and its standard error,
/// which goes to a file in `scratch`.
struct Said {
    std::optional<int> status;
    std::string err;
};

Said ask_status(const std::filesystem::path& scratch, const std::filesystem::path& dir) {
    const std::filesystem::path err = scratch / "play.err";
    std::filesystem::remove(err);
    Process play = ks::processes::test::start(
        "/bin/sh",
        ks::logs::test::with_stderr(err, KS_PLAY_PATH, {"--runtime-dir", dir.string(), "status"}));
    Said said{.status = play.wait(), .err = {}};
    std::ifstream in{err, std::ios::binary};
    said.err.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
    return said;
}

std::set<std::filesystem::path> files_in(const std::filesystem::path& dir) {
    std::set<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator{dir}) {
        files.insert(entry.path());
    }
    return files;
}

} // namespace

TEST_CASE("ks-play: an engine without kool_spin", "[crash][play]") {
    const ks::ipc::test::TempDir scratch;
    const ks::ipc::test::TempDir dir;

    SECTION("an orphan") {
        const Files files;
        auto orphan = orphaned(files);
        const std::filesystem::path where = orphan->session.dir();
        const auto before = files_in(where);
        const Said said = ask_status(scratch.path(), where);
        CHECK(said.status == 3);
        CAPTURE(said.err);
        CHECK(said.err.find(kHint) != std::string::npos);
        CHECK(said.err.find("(process " + std::to_string(orphan->session.engine()) + ")") !=
              std::string::npos);
        CHECK(files_in(where) == before);
        CHECK(keeps_beating(orphan->client.state));
    }

    SECTION("an engine lock that doesn't give its process") {
        const Process holder = hold_lock(dir.path(), "stops");
        write_lock(dir.path(), "");
        const Said said = ask_status(scratch.path(), dir.path());
        CHECK(said.status == 3);
        CHECK(said.err.find(kHint) != std::string::npos);
        CHECK(said.err.find("(process") == std::string::npos);
    }

    SECTION("nothing running") {
        const Said said = ask_status(scratch.path(), dir.path());
        CHECK(said.status == 3);
        CHECK(said.err.find(kHint) == std::string::npos);
        CHECK(files_in(dir.path()).empty());
    }

    SECTION("no runtime directory") {
        const Said said = ask_status(scratch.path(), dir.path() / "nowhere");
        CHECK(said.status == 3);
        CHECK(said.err.find(kHint) == std::string::npos);
        CHECK(files_in(dir.path()).empty());
    }

    SECTION("a supervisor lock held") {
        const std::expected<LockFile, LockFile::Failure> lock =
            LockFile::acquire(dir.path(), ks::ipc::kSupervisorLockName);
        REQUIRE(lock.has_value());
        const auto before = files_in(dir.path());
        const Said alone = ask_status(scratch.path(), dir.path());
        CHECK(alone.status == 3);
        CHECK(alone.err.find(kHint) == std::string::npos);
        CHECK(files_in(dir.path()) == before);
        // With an engine too: that's a kool_spin starting, or stopping.
        const Process holder = hold_lock(dir.path(), "stops");
        const Said both = ask_status(scratch.path(), dir.path());
        CHECK(both.status == 3);
        CHECK(both.err.find(kHint) == std::string::npos);
    }
}
