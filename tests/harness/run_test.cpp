// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for run(): what each option does, its output, and its exit
// codes (specs/harness/spec.md requirements 2 and 3).

#include <ks/engine/miniaudio_backend.h>
#include <ks/harness/options.h>
#include <ks/harness/script.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>

using namespace ks::harness;

namespace {

std::filesystem::path temp_path(std::string_view name) {
    return std::filesystem::temp_directory_path() / (std::string{"ks_harness_run_"} += name);
}

std::filesystem::path write_script(std::string_view name, std::string_view text) {
    const std::filesystem::path path = temp_path(name);
    std::ofstream file{path, std::ios::binary};
    file << text;
    return path;
}

struct Outcome {
    int code = -1;
    std::string out;
    std::string err;
};

Outcome run_with(const Options& options) {
    std::ostringstream out;
    std::ostringstream err;
    const std::atomic<bool> stop{false};
    const int code = run(options, out, err, stop);
    return {.code = code, .out = out.str(), .err = err.str()};
}

/// --check on `script`.
Options checking(const std::filesystem::path& script) {
    Options options;
    options.script = script;
    options.check = true;
    return options;
}

constexpr std::string_view kShort = "track tone sine 1000 2\n"
                                    "0 load 1 tone\n"
                                    "0 play 1\n"
                                    "0.5 rate 1 1.5\n"
                                    "1 end\n";

} // namespace

TEST_CASE("run(): --help prints the usage", "[harness][run]") {
    Options options;
    options.help = true;
    const Outcome outcome = run_with(options);
    CHECK(outcome.code == kExitOk);
    CHECK(outcome.out == usage());
}

TEST_CASE("run(): --list-devices numbers the devices and marks the default", "[harness][run]") {
    Options options;
    options.backend = ks::engine::MiniaudioBackends::Null;
    options.list_devices = true;
    const Outcome outcome = run_with(options);
    CHECK(outcome.code == kExitOk);
    CHECK(outcome.out.find("  0: NULL Playback Device (default)\n") != std::string::npos);
}

TEST_CASE("run(): --check prints a summary", "[harness][run]") {
    const auto path = write_script("check.harness", kShort);
    const Outcome outcome = run_with(checking(path));
    CHECK(outcome.code == kExitOk);
    CHECK(outcome.out.find("1 tracks, 3 events, ends at 1.000 s") != std::string::npos);
    CHECK(outcome.err.empty());
}

TEST_CASE("run(): script errors name the script and the line", "[harness][run]") {
    SECTION("a bad line") {
        const auto path = write_script("bad.harness", "track tone sine 1000 2\n0 jump 1\n1 end\n");
        const Outcome outcome = run_with(checking(path));
        CHECK(outcome.code == kExitUsageError);
        CHECK(outcome.err.starts_with(path.string() + ":2: "));
    }
    SECTION("a script over 1 MiB, as line 0") {
        const auto path =
            write_script("big.harness", std::string(kMaxScriptBytes + 1, '#') + "\n1 end\n");
        const Outcome outcome = run_with(checking(path));
        CHECK(outcome.code == kExitUsageError);
        CHECK(outcome.err.starts_with(path.string() + ":0: "));
    }
}

TEST_CASE("run(): runtime errors exit 1 and name what failed", "[harness][run]") {
    SECTION("a script that can't be opened") {
        const auto path = temp_path("no_such_script.harness");
        std::filesystem::remove(path);
        const Outcome outcome = run_with(checking(path));
        CHECK(outcome.code == kExitRuntimeError);
        CHECK(outcome.err.find(path.string()) != std::string::npos);
    }
    SECTION("a track file that can't be decoded") {
        const auto missing = temp_path("no_such_track.wav");
        std::filesystem::remove(missing);
        const auto path = write_script("missing_track.harness",
                                       "track t file \"" + missing.string() + "\"\n1 end\n");
        const Outcome outcome = run_with(checking(path));
        CHECK(outcome.code == kExitRuntimeError);
        CHECK(outcome.err.find(missing.string()) != std::string::npos);
    }
}

TEST_CASE("run(): --out renders the script and prints the report", "[harness][run]") {
    const auto path = write_script("render.harness", kShort);
    const auto wav = temp_path("render.wav");
    Options options;
    options.script = path;
    options.out = wav;
    const Outcome outcome = run_with(options);
    CHECK(outcome.code == kExitOk);
    CHECK(outcome.out.find("Played 1.000 s\n") != std::string::npos);
    CHECK(outcome.out.find("Commands: 2 sent, 2 read, 0 rejected, 0 delayed\n") !=
          std::string::npos);
    CHECK(std::filesystem::file_size(wav) > 0);
    std::filesystem::remove(wav);
}

TEST_CASE("run(): a device number that isn't listed is a usage error", "[harness][run]") {
    const auto path = write_script("device.harness", kShort);
    Options options;
    options.script = path;
    options.backend = ks::engine::MiniaudioBackends::Null;
    options.device = 1;
    const Outcome outcome = run_with(options);
    CHECK(outcome.code == kExitUsageError);
    CHECK(outcome.err.find("device 1 isn't listed") != std::string::npos);
}
