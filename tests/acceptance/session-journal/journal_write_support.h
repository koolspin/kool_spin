// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Helpers for the tests of kool_spin writing the session journal
// (specs/session-journal/spec.md requirements 8 to 16): reading the journal,
// what a file's status says, the boot ID, and sessions with a state folder.
// Built on the state-folder and engine-restart tests' helpers.

#include <ks/ipc/command.h>
#include <ks/ipc/state.h>
#include <ks/supervisor/journal.h>

#include "journal_support.h"
#include "state_support.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ks::session_journal::test {

using namespace ks::state_folder::test;
namespace jt = ks::supervisor::journal_test;
namespace restart = ks::engine_restart::test;
using supervisor::Journal;
using supervisor::JournalMixer;
using supervisor::JournalSide;

/// Waits are this many times longer under AddressSanitizer or
/// ThreadSanitizer, whose processes run several times slower.
inline constexpr int kSlack = kSlowSanitizer ? 3 : 1;

inline std::filesystem::path journal_path(const std::filesystem::path& state) {
    return state / supervisor::kJournalName;
}

/// The bytes of `path`; nothing if it can't be read.
inline std::optional<std::string> contents(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

inline void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(out.good());
}

/// The journal in `state`, parsed, if there is one and it parses.
inline std::optional<Journal> read_journal(const std::filesystem::path& state) {
    const std::optional<std::string> text = contents(journal_path(state));
    if (!text) {
        return std::nullopt;
    }
    std::expected<Journal, std::string> journal = jt::parse_text(*text);
    return journal ? std::optional<Journal>{std::move(*journal)} : std::nullopt;
}

/// A valid journal's text, written by no supervisor: an empty session with
/// the default mixer, from requirement 3.
inline std::string sample_text() {
    std::string body = "kool_spin session journal 1\nboot none\nengine 0\ntime 0\n";
    for (int channel = 1; channel <= 4; ++channel) {
        body += std::format("channel {0} trim-db 0\nchannel {0} fader 1\nchannel {0} side thru\n"
                            "channel {0} cue 0\n",
                            channel);
    }
    body += "crossfader 0.5\ncrossfader-transition 1\nmain-level-db 0\nheadphone-level-db 0\n"
            "cue-mix 0\nbus-main 0\nbus-cue 2\n";
    return body + jt::checksum_line(body);
}

/// What a file's status says: enough to tell whether it was replaced.
struct FileStatus {
    ino_t inode = 0;
    std::int64_t modified = 0; ///< Nanoseconds since the Unix epoch.
    std::uint64_t size = 0;
    unsigned mode = 0;

    bool operator==(const FileStatus&) const = default;
};

inline std::optional<FileStatus> status_of(const std::filesystem::path& path) {
    struct stat status{};
    if (::stat(path.c_str(), &status) != 0) {
        return std::nullopt;
    }
#ifdef __APPLE__
    const timespec time = status.st_mtimespec;
#else
    const timespec time = status.st_mtim;
#endif
    return FileStatus{.inode = status.st_ino,
                      .modified = (std::int64_t{time.tv_sec} * 1'000'000'000) + time.tv_nsec,
                      .size = static_cast<std::uint64_t>(status.st_size),
                      .mode = static_cast<unsigned>(status.st_mode & 07777U)};
}

/// This machine's boot ID, read as requirement 15 says, or nothing.
inline std::optional<std::string> machine_boot_id() {
    std::string id;
#ifdef __APPLE__
    std::array<char, 128> buffer{};
    std::size_t size = buffer.size();
    if (::sysctlbyname("kern.bootsessionuuid", buffer.data(), &size, nullptr, 0) != 0) {
        return std::nullopt;
    }
    id.assign(buffer.data());
#else
    const std::optional<std::string> text = contents("/proc/sys/kernel/random/boot_id");
    if (!text) {
        return std::nullopt;
    }
    id = *text;
    while (!id.empty() && (id.back() == '\n' || id.back() == ' ')) {
        id.pop_back();
    }
#endif
    std::ranges::transform(id, id.begin(),
                           [](char c) { return static_cast<char>(std::tolower(c)); });
    return id;
}

/// The mixer settings a MixerState block holds, as a journal holds them.
inline JournalMixer mixer_of(const ipc::MixerState& block) {
    JournalMixer mixer;
    for (std::size_t i = 0; i < supervisor::kJournalDecks; ++i) {
        const ipc::ChannelState& from = std::span{block.channels}[i];
        supervisor::JournalChannel& to = std::span{mixer.channels}[i];
        to.trim_db = restart::real_of(from.trim_db);
        to.fader = restart::real_of(from.fader);
        to.side = static_cast<JournalSide>(from.side);
        to.cue = from.cue != 0;
    }
    mixer.crossfader = restart::real_of(block.crossfader);
    mixer.crossfader_transition = restart::real_of(block.crossfader_transition);
    mixer.main_level_db = restart::real_of(block.main_level_db);
    mixer.headphone_level_db = restart::real_of(block.headphone_level_db);
    mixer.cue_mix = restart::real_of(block.cue_mix);
    mixer.bus_main = static_cast<std::uint32_t>(block.bus_main);
    mixer.bus_cue = static_cast<std::uint32_t>(block.bus_cue);
    return mixer;
}

/// Session options with the state folder `state` and `--journal-interval`.
inline restart::SessionOptions journaled(const std::filesystem::path& state,
                                         std::chrono::milliseconds interval,
                                         std::vector<std::string> more = {}) {
    restart::SessionOptions options;
    more.insert(more.begin(), {"--journal-interval", std::to_string(interval.count())});
    options.supervisor = with_state(state, std::move(more));
    return options;
}

/// True once every command `session` has sent through its ring has been read.
inline bool applied(const Playing& session) {
    return eventually(
        [&] { return session.client.state.ack(0).last_sequence + 1 >= session.sequence; });
}

/// How many lines of `session`'s kool_spin.log are at `level` and contain
/// `phrase`.
inline std::size_t lines_with(const restart::Session& session, std::string_view level,
                              std::string_view phrase) {
    const std::vector<std::string> lines = logs::test::read_lines(session.kool_spin_log());
    return static_cast<std::size_t>(std::ranges::count_if(lines, [&](const std::string& raw) {
        const std::optional<logs::test::Line> line = logs::test::parse_line(raw);
        return line && line->level == level && line->text.find(phrase) != std::string::npos;
    }));
}

} // namespace ks::session_journal::test
