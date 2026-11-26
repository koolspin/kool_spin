// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by the journal format tests and the journal fuzz target: a check of
// requirement 5's values written from the spec, independently of parse(); a
// generator of valid journals; text surgery that keeps the checksum right; and
// requirement 7's properties.

#include <ks/engine/resume.h>
#include <ks/ipc/crc32.h>
#include <ks/ipc/state.h>
#include <ks/ipc/track_segment.h>
#include <ks/supervisor/journal.h>

#include "../resume-state/snapshot_support.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ks::supervisor::journal_test {

using engine::resume_test::between;
using engine::resume_test::ByteReader;
using engine::resume_test::kInfinity;
using engine::resume_test::level;
using engine::resume_test::unit;

inline constexpr auto kMaxFrames = static_cast<double>(ipc::kMaxTrackFrames);
inline constexpr std::uint64_t kMaxSize = (std::uint64_t{1} << 63U) - 1;
inline constexpr std::int32_t kMaxPid = std::numeric_limits<std::int32_t>::max();

// ---- Bytes and text ---------------------------------------------------------------------

inline std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span{text.data(), text.size()});
}

/// `parse()` of `text`.
inline std::expected<Journal, std::string> parse_text(std::string_view text) {
    return parse(bytes_of(text));
}

/// A double as format() writes it: the shortest form that reads back exactly.
inline std::string real(double value) {
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), std::to_address(buffer.end()), value);
    return error == std::errc{} ? std::string{buffer.data(), end} : std::string{"?"};
}

/// The checksum line for `body`: requirement 3.
inline std::string checksum_line(std::string_view body) {
    return std::format("checksum {:08x}\n", ipc::crc32(bytes_of(body)));
}

/// `text` with its checksum made right: everything from the last line that
/// starts "checksum " is replaced by the checksum of what comes before it. A
/// text with no such line gets one.
inline std::string with_checksum(std::string_view text) {
    std::size_t start = std::string_view::npos;
    if (text.starts_with("checksum ")) {
        start = 0;
    }
    if (const std::size_t found = text.rfind("\nchecksum "); found != std::string_view::npos) {
        start = found + 1;
    }
    std::string body{start == std::string_view::npos ? text : text.substr(0, start)};
    if (!body.empty() && !body.ends_with('\n')) {
        body += '\n';
    }
    return body + checksum_line(body);
}

/// The lines of `text`, each without its "\n". A last line without one is
/// kept too.
inline std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            lines.emplace_back(text.substr(start));
            break;
        }
        lines.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

/// The lines joined, each ending in "\n".
inline std::string joined(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) {
        text += line;
        text += '\n';
    }
    return text;
}

/// `text` with the value of the line whose key is `key` (everything before
/// its last space) replaced by `value`, and the checksum made right. Empty if
/// there's no such line.
inline std::string with_value(std::string_view text, std::string_view key, std::string_view value) {
    std::vector<std::string> lines = lines_of(text);
    bool found = false;
    for (std::string& line : lines) {
        if (line.starts_with(key) && line.size() > key.size() && line[key.size()] == ' ' &&
            line.find(' ', key.size() + 1) == std::string::npos) {
            line = std::string{key} + " " + std::string{value};
            found = true;
            break;
        }
    }
    return found ? with_checksum(joined(lines)) : std::string{};
}

// ---- Requirement 5, from the spec ---------------------------------------------------------

/// Whether `text` is valid UTF-8: no overlong forms, no surrogates, nothing
/// above U+10FFFF.
inline bool valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        std::uint32_t code = 0;
        if (lead < 0x80U) {
            ++i;
            continue;
        }
        if (lead >= 0xC2U && lead <= 0xDFU) {
            extra = 1;
            code = lead & 0x1FU;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            extra = 2;
            code = lead & 0x0FU;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            extra = 3;
            code = lead & 0x07U;
        } else {
            return false;
        }
        if (i + extra >= text.size()) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code = (code << 6U) | (next & 0x3FU);
        }
        if ((extra == 2 && code < 0x800U) || (extra == 3 && code < 0x10000U) ||
            (code >= 0xD800U && code <= 0xDFFFU) || code > 0x10FFFFU) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

inline bool in(double value, double low, double high) {
    return value >= low && value <= high; // NaN fails
}

/// A trim or level: at most `high`, −∞ allowed, never NaN.
inline bool level_ok(double value, double high) {
    return value == -kInfinity || in(value, -std::numeric_limits<double>::max(), high);
}

inline bool side_ok(JournalSide side) {
    return side == JournalSide::A || side == JournalSide::B || side == JournalSide::Thru;
}

/// Whether every value of `journal` is one requirement 5 allows, and its boot
/// ID one requirement 3 does.
inline bool values_allowed(const Journal& journal) {
    if (journal.boot) {
        const std::string& boot = *journal.boot;
        if (boot.empty() || boot.size() > kMaxBootIdSize || !std::ranges::all_of(boot, [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-';
            })) {
            return false;
        }
    }
    if (journal.engine < 0 || journal.time < 0) {
        return false;
    }
    for (const std::optional<JournalEntry>& entry : journal.decks) {
        if (!entry) {
            continue;
        }
        if (entry->path.empty() || entry->path.size() > kMaxJournalPathSize ||
            entry->path.front() != '/' || !valid_utf8(entry->path) || entry->size > kMaxSize ||
            entry->sample_rate < ipc::kMinTrackRate || entry->sample_rate > ipc::kMaxTrackRate ||
            !in(entry->position, 0.0, kMaxFrames) || !in(entry->cue, 0.0, kMaxFrames) ||
            !in(entry->rate, -ipc::kMaxDeckRate, ipc::kMaxDeckRate)) {
            return false;
        }
    }
    const JournalMixer& mixer = journal.mixer;
    for (const JournalChannel& channel : mixer.channels) {
        if (!level_ok(channel.trim_db, ipc::kMaxTrimDb) || !in(channel.fader, 0.0, 1.0) ||
            !side_ok(channel.side)) {
            return false;
        }
    }
    return in(mixer.crossfader, 0.0, 1.0) &&
           in(mixer.crossfader_transition, ipc::kMinCrossfaderTransition, 1.0) &&
           level_ok(mixer.main_level_db, 0.0) && level_ok(mixer.headphone_level_db, 0.0) &&
           in(mixer.cue_mix, 0.0, 1.0) && mixer.bus_main % 2 == 0 && mixer.bus_cue % 2 == 0 &&
           mixer.bus_main < ipc::kMaxOutputChannels && mixer.bus_cue < ipc::kMaxOutputChannels &&
           mixer.bus_main != mixer.bus_cue;
}

// ---- A generator of valid journals ----------------------------------------------------------

/// Pieces a path is made of: plain ASCII, bytes that need an escape, and
/// multi-byte UTF-8.
inline constexpr std::array<std::string_view, 16> kPathPieces{"a",
                                                              "Z",
                                                              "9",
                                                              " ",
                                                              ".",
                                                              "/",
                                                              "-",
                                                              "\\",
                                                              "\t",
                                                              "\n",
                                                              "\x01",
                                                              "\x1f",
                                                              "\x7f",
                                                              "\xc3\xa9",
                                                              "\xe2\x82\xac",
                                                              "\xf0\x9d\x84\x9e"};

/// A valid path chosen by `in`: sometimes exactly kMaxJournalPathSize bytes.
inline std::string valid_path(ByteReader& in) {
    std::string path{"/"};
    const std::uint8_t kind = in.byte();
    const std::size_t pieces = kind % 4 == 0 ? 0 : 1 + (in.byte() % 48);
    for (std::size_t i = 0; i < pieces; ++i) {
        path += kPathPieces.at(in.byte() % kPathPieces.size());
    }
    if (kind % 16 == 1) {
        path.resize(kMaxJournalPathSize, 'x'); // pad with ASCII: still valid UTF-8
        if (!valid_utf8(path)) {
            path = "/" + std::string(kMaxJournalPathSize - 1, 'x');
        }
    }
    return path;
}

/// A valid boot ID chosen by `in`, or none.
inline std::optional<std::string> valid_boot(ByteReader& in) {
    const std::uint8_t kind = in.byte();
    if (kind % 4 == 0) {
        return std::nullopt;
    }
    constexpr std::string_view kChars = "0123456789abcdef-";
    const std::size_t size = kind % 4 == 1 ? kMaxBootIdSize : 1 + (in.byte() % kMaxBootIdSize);
    std::string boot;
    for (std::size_t i = 0; i < size; ++i) {
        boot += kChars.at(in.byte() % kChars.size());
    }
    return boot;
}

/// A valid journal chosen by `in`, the limits of requirement 5 included.
inline Journal valid_journal(ByteReader& in) {
    Journal journal;
    journal.boot = valid_boot(in);
    const std::uint8_t pick = in.byte();
    const std::uint64_t raw = in.word();
    switch (pick % 3) {
    case 0:
        journal.engine = 0;
        break;
    case 1:
        journal.engine = kMaxPid;
        break;
    default:
        journal.engine = static_cast<std::int32_t>(raw % (std::uint64_t{1} << 31U));
        break;
    }
    const std::uint64_t time = in.word();
    journal.time = (pick & 8U) != 0 ? 0 : static_cast<std::int64_t>(time >> 1U);
    for (std::optional<JournalEntry>& deck : journal.decks) {
        if (in.byte() % 3 == 0) {
            continue;
        }
        JournalEntry entry;
        entry.path = valid_path(in);
        const std::uint8_t sizes = in.byte();
        switch (sizes % 3) {
        case 0:
            entry.size = 0;
            break;
        case 1:
            entry.size = kMaxSize;
            break;
        default:
            entry.size = in.word() >> 1U;
            break;
        }
        entry.modified = std::bit_cast<std::int64_t>(in.word());
        entry.sample_rate =
            static_cast<std::uint32_t>(between(in, ipc::kMinTrackRate, ipc::kMaxTrackRate));
        entry.position = between(in, 0.0, kMaxFrames);
        entry.rate = between(in, -ipc::kMaxDeckRate, ipc::kMaxDeckRate);
        entry.cue = between(in, 0.0, kMaxFrames);
        entry.playing = (in.byte() & 1U) != 0;
        deck = std::move(entry);
    }
    JournalMixer& mixer = journal.mixer;
    for (JournalChannel& channel : mixer.channels) {
        channel.trim_db = level(in, ipc::kMaxTrimDb);
        channel.fader = between(in, 0.0, 1.0);
        const std::uint8_t flags = in.byte();
        constexpr std::array<JournalSide, 3> kSides{JournalSide::A, JournalSide::B,
                                                    JournalSide::Thru};
        channel.side = kSides.at(flags % 3);
        channel.cue = (flags & 8U) != 0;
    }
    mixer.crossfader = between(in, 0.0, 1.0);
    mixer.crossfader_transition = between(in, ipc::kMinCrossfaderTransition, 1.0);
    mixer.main_level_db = level(in, 0.0);
    mixer.headphone_level_db = level(in, 0.0);
    mixer.cue_mix = between(in, 0.0, 1.0);
    constexpr std::uint32_t kPairs = ipc::kMaxOutputChannels / 2;
    const std::uint64_t map = in.word();
    const auto main = static_cast<std::uint32_t>(map % kPairs);
    const auto cue = static_cast<std::uint32_t>((main + 1 + ((map >> 8U) % (kPairs - 1))) % kPairs);
    mixer.bus_main = 2 * main;
    mixer.bus_cue = 2 * cue;
    return journal;
}

/// Track IDs for resume_state(), chosen by `in`: some 0.
inline std::array<std::uint64_t, kJournalDecks> track_ids(ByteReader& in) {
    std::array<std::uint64_t, kJournalDecks> ids{};
    for (std::uint64_t& id : ids) {
        id = in.byte() % 3 == 0 ? 0 : 1 + (in.word() >> 1U);
    }
    return ids;
}

// ---- Requirement 7's properties ------------------------------------------------------------

/// What `resume_state()` of `journal` must be, as engine::validate() reads
/// it back: requirement 6.
inline engine::Snapshot expected_snapshot(const Journal& journal,
                                          std::span<const std::uint64_t, kJournalDecks> ids,
                                          std::uint64_t generation,
                                          std::chrono::steady_clock::time_point host_time) {
    engine::Snapshot snapshot;
    snapshot.generation = generation;
    snapshot.host_time = host_time;
    for (std::size_t i = 0; i < kJournalDecks; ++i) {
        const std::optional<JournalEntry>& entry = std::span{journal.decks}[i];
        engine::DeckSnapshot& deck = std::span{snapshot.decks}[i];
        if (!entry || ids[i] == 0) {
            continue;
        }
        deck.track_id = ids[i];
        deck.playing = false;
        deck.position = entry->position;
        deck.rate = entry->rate;
        deck.cue = entry->cue;
    }
    engine::MixerSettings& mixer = snapshot.mixer;
    for (std::size_t i = 0; i < kJournalDecks; ++i) {
        const JournalChannel& from = std::span{journal.mixer.channels}[i];
        engine::ChannelSettings& to = std::span{mixer.channels}[i];
        to.trim_db = from.trim_db;
        to.fader = from.fader;
        to.side = static_cast<engine::CrossfaderSide>(static_cast<std::uint8_t>(from.side));
        to.cue = from.cue;
    }
    mixer.crossfader = journal.mixer.crossfader;
    mixer.crossfader_transition = journal.mixer.crossfader_transition;
    mixer.main_level_db = journal.mixer.main_level_db;
    mixer.headphone_level_db = journal.mixer.headphone_level_db;
    mixer.cue_mix = journal.mixer.cue_mix;
    mixer.bus_map = engine::BusMap{.main = journal.mixer.bus_main, .cue = journal.mixer.bus_cue};
    return snapshot;
}

/// The resume slot of an accepted journal is one engine::validate() accepts,
/// and holds what requirement 6 says.
inline bool slot_check(const Journal& journal, std::span<const std::uint64_t, kJournalDecks> ids,
                       std::uint64_t generation) {
    const auto host_time = engine::resume_test::at_ns(123'456'789);
    const ipc::ResumeState words = resume_state(journal, ids, generation, host_time);
    const std::optional<engine::Snapshot> read =
        engine::validate(words, generation % ipc::kResumeSlots);
    return read.has_value() && engine::resume_test::same_snapshot(
                                   *read, expected_snapshot(journal, ids, generation, host_time));
}

/// Whatever parse() accepts of `text` formats back to it, has allowed values,
/// and restores through a valid slot.
inline bool text_check(std::string_view text, ByteReader& in) {
    const std::expected<Journal, std::string> journal = parse_text(text);
    if (!journal) {
        return !journal.error().empty();
    }
    const std::array<std::uint64_t, kJournalDecks> ids = track_ids(in);
    return format(*journal) == text && values_allowed(*journal) &&
           slot_check(*journal, ids, 1 + (in.word() % engine::kMaxGeneration));
}

/// A valid journal formats, parses back equal, and formats again the same;
/// and a one-byte change to its text, with the checksum made right, is
/// either refused or round-trips too.
inline bool journal_check(const Journal& journal, ByteReader& in) {
    const std::string text = format(journal);
    const std::expected<Journal, std::string> back = parse_text(text);
    if (!back || *back != journal || format(*back) != text) {
        return false;
    }
    if (text.empty()) {
        return false;
    }
    std::string changed = text;
    const std::size_t at = in.word() % changed.size();
    switch (in.byte() % 3) {
    case 0:
        changed[at] = static_cast<char>(in.byte());
        break;
    case 1:
        changed.insert(at, 1, static_cast<char>(in.byte()));
        break;
    default:
        changed.erase(at, 1);
        break;
    }
    return text_check(with_checksum(changed), in);
}

/// Requirement 7, for one input.
inline bool check_input(std::span<const std::uint8_t> input) {
    const std::string text(input.begin(), input.end());
    ByteReader ids{input};
    if (!text_check(text, ids) || !text_check(with_checksum(text), ids)) {
        return false;
    }
    ByteReader journal_bytes{input};
    const Journal journal = valid_journal(journal_bytes);
    return values_allowed(journal) && journal_check(journal, journal_bytes);
}

} // namespace ks::supervisor::journal_test
