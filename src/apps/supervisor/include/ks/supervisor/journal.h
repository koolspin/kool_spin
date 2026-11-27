// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The session journal (specs/session-journal/spec.md requirements 2 to 6, 12,
/// and 15): what it holds, its text, the resume slot it restores, and writing
/// it to the state folder atomically.

#include <ks/ipc/state.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace ks::supervisor {

/// The decks a journal describes.
inline constexpr std::size_t kJournalDecks = ipc::kStateDecks;
/// The most bytes parse() reads (requirement 4).
inline constexpr std::size_t kMaxJournalSize = std::size_t{128} * 1024;
/// The longest path, in bytes, once unescaped: as for LoadFile.
inline constexpr std::size_t kMaxJournalPathSize = 4'096;
/// The longest boot ID (requirement 3).
inline constexpr std::size_t kMaxBootIdSize = 64;

/// The journal's file name in the state folder, the file it's written to
/// first, and the name a refused one is kept under (requirements 12 and 18).
inline constexpr std::string_view kJournalName = "session.journal";
inline constexpr std::string_view kJournalTempName = "session.journal.tmp";
inline constexpr std::string_view kRejectedJournalName = "session.journal.rejected";

/// How often the supervisor records the session: the default for
/// --journal-interval (requirement 9).
inline constexpr std::chrono::milliseconds kJournalInterval{1'000};
/// How long a stopping supervisor waits for the last write (requirement 14).
inline constexpr std::chrono::seconds kJournalExitTimeout{2};
/// How long restoring waits for the workers' probes, from the first worker's
/// start (requirement 20).
inline constexpr std::chrono::seconds kRestoreProbeTimeout{3};

/// Which side of the crossfader a channel is on, as MixerState codes it.
enum class JournalSide : std::uint8_t {
    A = 0,
    B = 1,
    Thru = 2,
};

/// One deck's entry (requirement 2).
struct JournalEntry {
    /// Absolute, valid UTF-8, 1 to kMaxJournalPathSize bytes.
    std::string path;
    /// The file's size in bytes, and its modification time in nanoseconds
    /// since the Unix epoch, when the supervisor opened it.
    std::uint64_t size = 0;
    std::int64_t modified = 0;
    /// The track's sample rate: kMinTrackRate to kMaxTrackRate.
    std::uint32_t sample_rate = 0;
    /// In track frames.
    double position = 0.0;
    double rate = 1.0;
    /// In track frames.
    double cue = 0.0;
    bool playing = false;

    bool operator==(const JournalEntry&) const = default;
};

/// One mixer channel. The defaults are the engine's.
struct JournalChannel {
    double trim_db = 0.0;
    double fader = 1.0;
    JournalSide side = JournalSide::Thru;
    bool cue = false;

    bool operator==(const JournalChannel&) const = default;
};

/// The mixer's settings. The defaults are the engine's.
struct JournalMixer {
    std::array<JournalChannel, kJournalDecks> channels{};
    double crossfader = 0.5;
    double crossfader_transition = 1.0;
    double main_level_db = 0.0;
    double headphone_level_db = 0.0;
    double cue_mix = 0.0;
    /// The buses' first device channels.
    std::uint32_t bus_main = 0;
    std::uint32_t bus_cue = 2;

    bool operator==(const JournalMixer&) const = default;
};

/// A session, as the journal holds it (requirement 2). A default Journal is an
/// empty session, with the engine's default mixer, written by no engine.
struct Journal {
    /// The boot ID of the machine that wrote it; nothing for `none`.
    std::optional<std::string> boot;
    /// The process ID of the engine whose state it records; 0 for none.
    std::int32_t engine = 0;
    /// The steady clock's time of that state, in nanoseconds since its epoch.
    std::int64_t time = 0;
    std::array<std::optional<JournalEntry>, kJournalDecks> decks{};
    JournalMixer mixer{};

    bool operator==(const Journal&) const = default;
};

/// Whether every value of `journal` is one requirement 5 allows, and its boot
/// ID one requirement 3 does: what format() needs, and parse() gives.
[[nodiscard]] bool allowed(const Journal& journal);

/// The journal's text (requirement 3). @pre every value is one requirement 5
/// allows.
[[nodiscard]] std::string format(const Journal& journal);

/// The journal `text` holds, if it's text that format() could have written,
/// with values requirement 5 allows; otherwise why not (requirement 4). A pure
/// function, safe for any bytes.
[[nodiscard]] std::expected<Journal, std::string> parse(std::span<const std::byte> text);

/// The resume slot that restores `journal` (requirement 6): each deck with an
/// entry and a non-zero ID in `track_ids` gets that track, paused; the others
/// are empty. @pre generation is 1 to 2⁶³ − 1; host_time is at or after the
/// steady clock's epoch.
[[nodiscard]] ipc::ResumeState resume_state(const Journal& journal,
                                            std::span<const std::uint64_t, kJournalDecks> track_ids,
                                            std::uint64_t generation,
                                            std::chrono::steady_clock::time_point host_time);

/// This machine's boot ID, as a journal's `boot` holds it, or nothing if it
/// can't be read or isn't valid there (requirement 15).
[[nodiscard]] std::optional<std::string> boot_id();

/// Writes `text` to `dir`/kJournalName atomically (requirement 12): to
/// kJournalTempName, synced, renamed over it, and the folder synced. Blocks
/// for as long as the disk takes; the supervisor calls it from its writer
/// thread.
[[nodiscard]] std::expected<void, std::error_code> write_journal(const std::filesystem::path& dir,
                                                                 std::string_view text);

} // namespace ks::supervisor
