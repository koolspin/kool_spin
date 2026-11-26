// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/supervisor/journal.h>

#include <ks/ipc/crc32.h>
#include <ks/ipc/fd.h>
#include <ks/ipc/state.h>
#include <ks/ipc/track_segment.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace ks::supervisor {

namespace {

constexpr std::string_view kVersionLine = "kool_spin session journal 1";
constexpr std::string_view kNone = "none";
constexpr auto kMaxFrames = static_cast<double>(ipc::kMaxTrackFrames);
constexpr std::uint64_t kMaxInt63 = (std::uint64_t{1} << 63U) - 1;
constexpr double kInfinity = std::numeric_limits<double>::infinity();

// ---- Values (requirement 5) ---------------------------------------------------------

bool within(double value, double low, double high) {
    return value >= low && value <= high; // NaN fails
}

/// A trim or level: at most `high`, −∞ allowed, never NaN.
bool level_ok(double value, double high) {
    return value == -kInfinity || within(value, std::numeric_limits<double>::lowest(), high);
}

bool boot_ok(std::string_view boot) {
    return !boot.empty() && boot.size() <= kMaxBootIdSize && std::ranges::all_of(boot, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-';
    });
}

/// Whether `text` is valid UTF-8: no overlong forms, surrogates, or code
/// points above U+10FFFF.
bool valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80U) {
            ++i;
            continue;
        }
        std::size_t extra = 0;
        std::uint32_t code = 0;
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

bool entry_ok(const JournalEntry& entry) {
    return !entry.path.empty() && entry.path.size() <= kMaxJournalPathSize &&
           entry.path.front() == '/' && valid_utf8(entry.path) && entry.size <= kMaxInt63 &&
           entry.sample_rate >= ipc::kMinTrackRate && entry.sample_rate <= ipc::kMaxTrackRate &&
           within(entry.position, 0.0, kMaxFrames) && within(entry.cue, 0.0, kMaxFrames) &&
           within(entry.rate, -ipc::kMaxDeckRate, ipc::kMaxDeckRate);
}

bool side_ok(JournalSide side) {
    return side == JournalSide::A || side == JournalSide::B || side == JournalSide::Thru;
}

bool mixer_ok(const JournalMixer& mixer) {
    const bool channels = std::ranges::all_of(mixer.channels, [](const JournalChannel& c) {
        return level_ok(c.trim_db, ipc::kMaxTrimDb) && within(c.fader, 0.0, 1.0) && side_ok(c.side);
    });
    return channels && within(mixer.crossfader, 0.0, 1.0) &&
           within(mixer.crossfader_transition, ipc::kMinCrossfaderTransition, 1.0) &&
           level_ok(mixer.main_level_db, 0.0) && level_ok(mixer.headphone_level_db, 0.0) &&
           within(mixer.cue_mix, 0.0, 1.0) && mixer.bus_main % 2 == 0 && mixer.bus_cue % 2 == 0 &&
           mixer.bus_main < ipc::kMaxOutputChannels && mixer.bus_cue < ipc::kMaxOutputChannels &&
           mixer.bus_main != mixer.bus_cue;
}

// ---- Writing ----------------------------------------------------------------------

/// The shortest text that reads back as exactly `value`; −∞ is "-inf".
std::string real_text(double value) {
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), std::to_address(buffer.end()), value);
    return error == std::errc{} ? std::string{buffer.data(), end} : std::string{};
}

/// `path` with `\` doubled, and control bytes as `\xHH` (requirement 3).
std::string escaped(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    for (const char c : path) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '\\') {
            out += "\\\\";
        } else if (byte < 0x20U || byte == 0x7FU) {
            out += std::format("\\x{:02x}", byte);
        } else {
            out += c;
        }
    }
    return out;
}

std::string_view side_text(JournalSide side) {
    switch (side) {
    case JournalSide::A:
        return "a";
    case JournalSide::B:
        return "b";
    case JournalSide::Thru:
        break;
    }
    return "thru";
}

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span{text.data(), text.size()});
}

std::string checksum_text(std::string_view body) {
    return std::format("{:08x}", ipc::crc32(bytes_of(body)));
}

// ---- Reading ----------------------------------------------------------------------

std::string bad(std::string_view key, std::string_view why) {
    return std::format("'{}' {}", key, why);
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    return -1;
}

/// Reads a journal's lines in order, refusing anything format() wouldn't
/// write. The first thing wrong is kept; after it, every read returns a
/// default and changes nothing.
class Reader {
public:
    explicit Reader(std::string_view body) : rest_{body} {}

    [[nodiscard]] const std::optional<std::string>& error() const noexcept { return error_; }
    [[nodiscard]] bool done() const noexcept { return rest_.empty(); }
    [[nodiscard]] bool next_starts(std::string_view prefix) const {
        return !error_ && rest_.starts_with(prefix);
    }
    void fail(std::string why) {
        if (!error_) {
            error_ = std::move(why);
        }
    }

    /// The next line, which must be exactly `line`.
    void exact(std::string_view line) {
        const std::optional<std::string_view> got = take();
        if (!got || *got != line) {
            fail(std::format("expected the line '{}'", line));
        }
    }

    /// The value of the next line, whose key must be `key`.
    std::string_view value(std::string_view key) {
        const std::optional<std::string_view> got = take();
        if (!got || !got->starts_with(key) || got->size() <= key.size() ||
            (*got)[key.size()] != ' ') {
            fail(std::format("expected a '{}' line", key));
            return {};
        }
        return got->substr(key.size() + 1);
    }

    /// An unsigned integer as format() writes it, at most `max`.
    std::uint64_t whole(std::string_view key, std::uint64_t max) {
        const std::string_view text = value(key);
        if (error_) {
            return 0;
        }
        if (text.empty() || (text.size() > 1 && text.front() == '0') ||
            !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
            fail(bad(key, "isn't a whole number as the journal writes one"));
            return 0;
        }
        std::uint64_t number = 0;
        const char* const last = std::to_address(text.end());
        const auto [end, failed] = std::from_chars(std::to_address(text.begin()), last, number);
        if (failed != std::errc{} || end != last || number > max) {
            fail(bad(key, "is out of range"));
            return 0;
        }
        return number;
    }

    /// A signed 64-bit integer as format() writes it.
    std::int64_t signed_whole(std::string_view key) {
        const std::string_view text = value(key);
        if (error_) {
            return 0;
        }
        const bool negative = text.starts_with('-');
        const std::string_view digits = negative ? text.substr(1) : text;
        if (digits.empty() || (digits.size() > 1 && digits.front() == '0') ||
            (negative && digits == "0") ||
            !std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; })) {
            fail(bad(key, "isn't a whole number as the journal writes one"));
            return 0;
        }
        std::int64_t number = 0;
        const char* const last = std::to_address(text.end());
        const auto [end, failed] = std::from_chars(std::to_address(text.begin()), last, number);
        if (failed != std::errc{} || end != last) {
            fail(bad(key, "is out of range"));
            return 0;
        }
        return number;
    }

    /// A real as format() writes it: the shortest text that reads back
    /// exactly, or "-inf" where `minus_infinity` allows it. Never NaN or +∞.
    double real(std::string_view key, bool minus_infinity = false) {
        const std::string_view text = value(key);
        if (error_) {
            return 0.0;
        }
        if (text == "-inf" && minus_infinity) {
            return -kInfinity;
        }
        // Only what to_chars() writes for a finite double; then strtod(),
        // since not every standard library parses a double with from_chars().
        if (text.empty() || text.size() > 40 || !std::ranges::all_of(text, [](char c) {
                return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e';
            })) {
            fail(bad(key, "isn't a number as the journal writes one"));
            return 0.0;
        }
        const std::string copy{text};
        // NOLINTNEXTLINE(misc-const-correctness): std::strtod's out-parameter is char**
        char* end = nullptr;
        const double number = std::strtod(copy.c_str(), &end);
        if (end != std::to_address(copy.end()) || !std::isfinite(number) ||
            real_text(number) != text) {
            fail(bad(key, "isn't a number as the journal writes one"));
            return 0.0;
        }
        return number;
    }

    bool flag(std::string_view key) {
        const std::string_view text = value(key);
        if (!error_ && text != "0" && text != "1") {
            fail(bad(key, "isn't 0 or 1"));
        }
        return text == "1";
    }

    JournalSide side(std::string_view key) {
        const std::string_view text = value(key);
        if (text == "a") {
            return JournalSide::A;
        }
        if (text == "b") {
            return JournalSide::B;
        }
        if (!error_ && text != "thru") {
            fail(bad(key, "isn't a, b, or thru"));
        }
        return JournalSide::Thru;
    }

    /// The path a line escapes, if format() could have written it.
    std::string path(std::string_view key) {
        const std::string_view text = value(key);
        if (error_) {
            return {};
        }
        std::string path;
        path.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20U || byte == 0x7FU) {
                fail(bad(key, "has a control character that isn't escaped"));
                return {};
            }
            if (c != '\\') {
                path += c;
                continue;
            }
            if (i + 1 < text.size() && text[i + 1] == '\\') {
                path += '\\';
                ++i;
                continue;
            }
            if (i + 3 < text.size() && text[i + 1] == 'x') {
                const int high = hex_digit(text[i + 2]);
                const int low = hex_digit(text[i + 3]);
                if (high >= 0 && low >= 0) {
                    const int code = (high * 16) + low;
                    if (code < 0x20 || code == 0x7F) {
                        path += static_cast<char>(code);
                        i += 3;
                        continue;
                    }
                }
            }
            fail(bad(key, "has an escape the journal doesn't write"));
            return {};
        }
        if (path.empty() || path.size() > kMaxJournalPathSize || path.front() != '/' ||
            !valid_utf8(path)) {
            fail(bad(key, "isn't an absolute path of valid UTF-8, at most 4096 bytes long"));
            return {};
        }
        return path;
    }

private:
    std::optional<std::string_view> take() {
        if (error_) {
            return std::nullopt;
        }
        const std::size_t end = rest_.find('\n');
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view line = rest_.substr(0, end);
        rest_.remove_prefix(end + 1);
        return line;
    }

    std::string_view rest_;
    std::optional<std::string> error_;
};

JournalEntry entry_of(Reader& reader, std::size_t deck) {
    JournalEntry entry;
    const std::string prefix = std::format("deck {} ", deck + 1);
    const auto key = [&](std::string_view name) { return prefix + std::string{name}; };
    entry.path = reader.path(key("path"));
    entry.size = reader.whole(key("size"), kMaxInt63);
    entry.modified = reader.signed_whole(key("modified"));
    entry.sample_rate =
        static_cast<std::uint32_t>(reader.whole(key("sample-rate"), ipc::kMaxTrackRate));
    entry.position = reader.real(key("position"));
    entry.rate = reader.real(key("rate"));
    entry.cue = reader.real(key("cue"));
    entry.playing = reader.flag(key("playing"));
    if (!reader.error() && !entry_ok(entry)) {
        reader.fail(std::format("deck {}'s values are out of range", deck + 1));
    }
    return entry;
}

JournalMixer mixer_of(Reader& reader) {
    JournalMixer mixer;
    for (std::size_t i = 0; i < kJournalDecks; ++i) {
        JournalChannel& channel = std::span{mixer.channels}[i];
        const std::string prefix = std::format("channel {} ", i + 1);
        const auto key = [&](std::string_view name) { return prefix + std::string{name}; };
        channel.trim_db = reader.real(key("trim-db"), true);
        channel.fader = reader.real(key("fader"));
        channel.side = reader.side(key("side"));
        channel.cue = reader.flag(key("cue"));
    }
    mixer.crossfader = reader.real("crossfader");
    mixer.crossfader_transition = reader.real("crossfader-transition");
    mixer.main_level_db = reader.real("main-level-db", true);
    mixer.headphone_level_db = reader.real("headphone-level-db", true);
    mixer.cue_mix = reader.real("cue-mix");
    mixer.bus_main = static_cast<std::uint32_t>(reader.whole("bus-main", ipc::kMaxOutputChannels));
    mixer.bus_cue = static_cast<std::uint32_t>(reader.whole("bus-cue", ipc::kMaxOutputChannels));
    if (!reader.error() && !mixer_ok(mixer)) {
        reader.fail("the mixer's settings are out of range");
    }
    return mixer;
}

std::error_code last_error() {
    return {errno, std::system_category()};
}

/// Syncs `fd` to the disk: on macOS with F_FULLFSYNC, falling back to
/// fsync() (requirement 12).
bool sync(int fd) {
#ifdef __APPLE__
    if (::fcntl(fd, F_FULLFSYNC) == 0) {
        return true;
    }
#endif
    int result = ::fsync(fd);
    while (result == -1 && errno == EINTR) {
        result = ::fsync(fd);
    }
    return result == 0;
}

} // namespace

bool allowed(const Journal& journal) {
    if ((journal.boot && !boot_ok(*journal.boot)) || journal.engine < 0 || journal.time < 0) {
        return false;
    }
    return std::ranges::all_of(journal.decks,
                               [](const std::optional<JournalEntry>& entry) {
                                   return !entry || entry_ok(*entry);
                               }) &&
           mixer_ok(journal.mixer);
}

std::string format(const Journal& journal) {
    std::string out;
    out.reserve(2'048);
    out += kVersionLine;
    out += '\n';
    out += std::format("boot {}\n", journal.boot ? std::string_view{*journal.boot} : kNone);
    out += std::format("engine {}\ntime {}\n", journal.engine, journal.time);
    for (std::size_t i = 0; i < kJournalDecks; ++i) {
        const std::optional<JournalEntry>& entry = std::span{journal.decks}[i];
        if (!entry) {
            continue;
        }
        const std::size_t n = i + 1;
        out += std::format("deck {} path {}\n", n, escaped(entry->path));
        out += std::format("deck {} size {}\n", n, entry->size);
        out += std::format("deck {} modified {}\n", n, entry->modified);
        out += std::format("deck {} sample-rate {}\n", n, entry->sample_rate);
        out += std::format("deck {} position {}\n", n, real_text(entry->position));
        out += std::format("deck {} rate {}\n", n, real_text(entry->rate));
        out += std::format("deck {} cue {}\n", n, real_text(entry->cue));
        out += std::format("deck {} playing {}\n", n, entry->playing ? 1 : 0);
    }
    const JournalMixer& mixer = journal.mixer;
    for (std::size_t i = 0; i < kJournalDecks; ++i) {
        const JournalChannel& channel = std::span{mixer.channels}[i];
        const std::size_t n = i + 1;
        out += std::format("channel {} trim-db {}\n", n, real_text(channel.trim_db));
        out += std::format("channel {} fader {}\n", n, real_text(channel.fader));
        out += std::format("channel {} side {}\n", n, side_text(channel.side));
        out += std::format("channel {} cue {}\n", n, channel.cue ? 1 : 0);
    }
    out += std::format("crossfader {}\n", real_text(mixer.crossfader));
    out += std::format("crossfader-transition {}\n", real_text(mixer.crossfader_transition));
    out += std::format("main-level-db {}\n", real_text(mixer.main_level_db));
    out += std::format("headphone-level-db {}\n", real_text(mixer.headphone_level_db));
    out += std::format("cue-mix {}\n", real_text(mixer.cue_mix));
    out += std::format("bus-main {}\nbus-cue {}\n", mixer.bus_main, mixer.bus_cue);
    out += std::format("checksum {}\n", checksum_text(out));
    return out;
}

std::expected<Journal, std::string> parse(std::span<const std::byte> text) {
    if (text.size() > kMaxJournalSize) {
        return std::unexpected{std::format("it's larger than {} bytes", kMaxJournalSize)};
    }
    std::string copy(text.size(), '\0');
    if (!text.empty()) {
        std::memcpy(copy.data(), text.data(), text.size());
    }
    const std::string_view all{copy};

    // The checksum line is the last, and covers everything before it.
    if (!all.ends_with('\n')) {
        return std::unexpected{std::string{"it doesn't end with a newline"}};
    }
    const std::size_t last = all.rfind('\n', all.size() - 2);
    const std::size_t start = last == std::string_view::npos ? 0 : last + 1;
    const std::string_view body = all.substr(0, start);
    const std::string_view checksum_line = all.substr(start, all.size() - start - 1);
    if (checksum_line != std::format("checksum {}", checksum_text(body))) {
        return std::unexpected{std::string{"its checksum is wrong"}};
    }

    Reader reader{body};
    Journal journal;
    reader.exact(kVersionLine);
    const std::string_view boot = reader.value("boot");
    if (!reader.error() && boot != kNone) {
        if (boot_ok(boot)) {
            journal.boot = std::string{boot};
        } else {
            reader.fail(bad("boot", "isn't a boot ID"));
        }
    }
    journal.engine = static_cast<std::int32_t>(reader.whole(
        "engine", static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())));
    journal.time = static_cast<std::int64_t>(reader.whole("time", kMaxInt63));

    // Each deck with an entry, in order.
    std::size_t next = 0;
    while (reader.next_starts("deck ")) {
        std::size_t deck = kJournalDecks;
        for (std::size_t i = next; i < kJournalDecks; ++i) {
            if (reader.next_starts(std::format("deck {} path ", i + 1))) {
                deck = i;
                break;
            }
        }
        if (deck == kJournalDecks) {
            reader.fail("its deck lines are out of order");
            break;
        }
        std::span{journal.decks}[deck] = entry_of(reader, deck);
        next = deck + 1;
    }
    journal.mixer = mixer_of(reader);
    if (!reader.error() && !reader.done()) {
        reader.fail("it has lines after the mixer's");
    }
    if (reader.error()) {
        return std::unexpected{*reader.error()};
    }
    return journal;
}

ipc::ResumeState resume_state(const Journal& journal,
                              std::span<const std::uint64_t, kJournalDecks> track_ids,
                              std::uint64_t generation,
                              std::chrono::steady_clock::time_point host_time) {
    ipc::ResumeState words{};
    words.generation = generation;
    words.host_time = std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(host_time.time_since_epoch())
            .count()));
    for (std::size_t i = 0; i < kJournalDecks; ++i) {
        const std::optional<JournalEntry>& entry = std::span{journal.decks}[i];
        ipc::ResumeDeck& deck = std::span{words.decks}[i];
        const std::uint64_t id = track_ids[i];
        if (!entry || id == 0) {
            deck = ipc::ResumeDeck{.track_id = 0,
                                   .flags = 0,
                                   .position = std::bit_cast<std::uint64_t>(0.0),
                                   .rate = std::bit_cast<std::uint64_t>(1.0),
                                   .cue = std::bit_cast<std::uint64_t>(0.0)};
            continue;
        }
        deck = ipc::ResumeDeck{.track_id = id,
                               .flags = 0,
                               .position = std::bit_cast<std::uint64_t>(entry->position),
                               .rate = std::bit_cast<std::uint64_t>(entry->rate),
                               .cue = std::bit_cast<std::uint64_t>(entry->cue)};
    }
    for (std::size_t i = 0; i < kJournalDecks; ++i) {
        const JournalChannel& from = std::span{journal.mixer.channels}[i];
        ipc::ChannelState& to = std::span{words.channels}[i];
        to.trim_db = std::bit_cast<std::uint64_t>(from.trim_db);
        to.fader = std::bit_cast<std::uint64_t>(from.fader);
        to.side = static_cast<std::uint64_t>(from.side);
        to.cue = from.cue ? 1 : 0;
    }
    words.crossfader = std::bit_cast<std::uint64_t>(journal.mixer.crossfader);
    words.crossfader_transition = std::bit_cast<std::uint64_t>(journal.mixer.crossfader_transition);
    words.main_level_db = std::bit_cast<std::uint64_t>(journal.mixer.main_level_db);
    words.headphone_level_db = std::bit_cast<std::uint64_t>(journal.mixer.headphone_level_db);
    words.cue_mix = std::bit_cast<std::uint64_t>(journal.mixer.cue_mix);
    words.bus_main = journal.mixer.bus_main;
    words.bus_cue = journal.mixer.bus_cue;
    const auto bytes = std::bit_cast<std::array<std::byte, sizeof(ipc::ResumeState)>>(words);
    words.checksum = ipc::crc32(std::span{bytes}.first(offsetof(ipc::ResumeState, checksum)));
    return words;
}

std::optional<std::string> boot_id() {
    std::string id;
#ifdef __APPLE__
    std::array<char, 128> buffer{};
    std::size_t size = buffer.size();
    if (::sysctlbyname("kern.bootsessionuuid", buffer.data(), &size, nullptr, 0) != 0) {
        return std::nullopt;
    }
    id.assign(buffer.data(), ::strnlen(buffer.data(), buffer.size()));
#else
    std::ifstream in{"/proc/sys/kernel/random/boot_id"};
    if (!in) {
        return std::nullopt;
    }
    std::getline(in, id);
#endif
    std::ranges::transform(id, id.begin(), [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    });
    if (!boot_ok(id)) {
        return std::nullopt;
    }
    return id;
}

std::expected<void, std::error_code> write_journal(const std::filesystem::path& dir,
                                                   std::string_view text) {
    const std::filesystem::path temp = dir / kJournalTempName;
    int fd = -1;
    for (;;) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open() is variadic
        fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd != -1 || errno != EINTR) {
            break;
        }
    }
    if (fd == -1) {
        return std::unexpected{last_error()};
    }
    ipc::Fd file{fd};
    const auto fail = [&temp](std::error_code error) -> std::expected<void, std::error_code> {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return std::unexpected{error};
    };
    // A file left by an earlier write may have another mode.
    if (::fchmod(fd, 0600) != 0) {
        return fail(last_error());
    }
    std::string_view rest = text;
    while (!rest.empty()) {
        const ssize_t n = ::write(fd, rest.data(), rest.size());
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return fail(n < 0 ? last_error() : std::make_error_code(std::errc::io_error));
        }
        rest.remove_prefix(static_cast<std::size_t>(n));
    }
    if (!sync(fd)) {
        return fail(last_error());
    }
    file.reset();
    if (::rename(temp.c_str(), (dir / kJournalName).c_str()) != 0) {
        return fail(last_error());
    }
    for (;;) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open() is variadic
        fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd != -1 || errno != EINTR) {
            break;
        }
    }
    if (fd == -1) {
        return std::unexpected{last_error()};
    }
    const ipc::Fd folder{fd};
    if (!sync(fd)) {
        return std::unexpected{last_error()};
    }
    return {};
}

} // namespace ks::supervisor
