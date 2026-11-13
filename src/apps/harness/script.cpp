// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/harness/script.h>

#include <ks/engine/block.h>
#include <ks/engine/command.h>
#include <ks/engine/mixer.h>
#include <ks/engine/track.h>
#include <ks/harness/encode.h>
#include <ks/ipc/command.h>

#include "script_lexer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ks::harness {

namespace {

using engine::Command;
using ipc::CommandType;
using lexer::Token;
using Tokens = std::span<const Token>;

/// A line's own error, in words; the caller adds the line.
using LineError = std::unexpected<std::string>;

template <typename T>
using Parsed = std::expected<T, std::string>;

LineError line_error(std::string message) {
    return LineError{std::move(message)};
}

std::string quoted(std::string_view text) {
    return "\"" + std::string{text} + "\"";
}

/// The unquoted text of `token`, or an error: only paths are quoted.
Parsed<std::string_view> word(const Token& token) {
    if (token.quoted) {
        return line_error("only a file's path may be in quotes");
    }
    return token.text;
}

Parsed<double> amount(const Token& token, std::string_view what) {
    const Parsed<std::string_view> text = word(token);
    if (!text) {
        return std::unexpected{text.error()};
    }
    if (const std::optional<double> parsed = lexer::parse_amount(*text)) {
        return *parsed;
    }
    return line_error(std::string{what} + " must be a number such as 12 or 0.25, not " +
                      quoted(*text));
}

Parsed<double> value(const Token& token, std::string_view what) {
    const Parsed<std::string_view> text = word(token);
    if (!text) {
        return std::unexpected{text.error()};
    }
    if (const std::optional<double> parsed = lexer::parse_value(*text)) {
        return *parsed;
    }
    return line_error(std::string{what} + " must be a number such as 1, -0.5 or 0.25, not " +
                      quoted(*text));
}

Parsed<double> decibels(const Token& token, std::string_view what) {
    const Parsed<std::string_view> text = word(token);
    if (!text) {
        return std::unexpected{text.error()};
    }
    if (const std::optional<double> parsed = lexer::parse_db(*text)) {
        return *parsed;
    }
    return line_error(std::string{what} + " must be a number of dB such as -6 or -inf, not " +
                      quoted(*text));
}

/// A whole number from `lo` to `hi`.
Parsed<std::uint64_t> whole(const Token& token, std::string_view what, std::uint64_t lo,
                            std::uint64_t hi) {
    const Parsed<std::string_view> text = word(token);
    if (!text) {
        return std::unexpected{text.error()};
    }
    const std::optional<std::uint64_t> parsed = lexer::parse_whole(*text, hi);
    if (!parsed || *parsed < lo) {
        return line_error(std::string{what} + " must be a whole number from " + std::to_string(lo) +
                          " to " + std::to_string(hi) + ", not " + quoted(*text));
    }
    return *parsed;
}

/// A deck, 1 to kDecks in the script, as a 0-based index.
Parsed<std::size_t> deck(const Token& token) {
    const Parsed<std::uint64_t> number = whole(token, "the deck", 1, engine::kDecks);
    if (!number) {
        return std::unexpected{number.error()};
    }
    return static_cast<std::size_t>(*number - 1);
}

/// An output channel, 1 to kMaxOutputChannels in the script, as a 0-based
/// index.
Parsed<std::size_t> output_channel(const Token& token) {
    const Parsed<std::uint64_t> number =
        whole(token, "an output channel", 1, engine::kMaxOutputChannels);
    if (!number) {
        return std::unexpected{number.error()};
    }
    return static_cast<std::size_t>(*number - 1);
}

std::string describe(engine::Rejection rejection) {
    switch (rejection) {
    case engine::Rejection::UnknownType:
        return "the engine doesn't know this command";
    case engine::Rejection::BadTarget:
        return "the engine would reject this command's deck";
    case engine::Rejection::NonzeroPadding:
        return "the engine would reject this command's layout";
    case engine::Rejection::BadValue:
        return "the engine would reject this command's value";
    }
    return "the engine would reject this command";
}

/// `command` as decode() returns it after encode(), or why decode() rejects
/// it (requirement 11).
Parsed<Command> checked(const Command& command) {
    const std::expected<Command, engine::Rejection> decoded = engine::decode(encode(command, 1));
    if (!decoded) {
        return line_error(describe(decoded.error()));
    }
    return *decoded;
}

Parsed<Command> deck_value(CommandType type, Tokens args, std::string_view what,
                           Parsed<double> (*parse)(const Token&, std::string_view)) {
    const Parsed<std::size_t> target = deck(args[0]);
    if (!target) {
        return std::unexpected{target.error()};
    }
    const Parsed<double> number = parse(args[1], what);
    if (!number) {
        return std::unexpected{number.error()};
    }
    return checked(Command{.type = type, .deck = *target, .value = *number});
}

Parsed<Command> global_value(CommandType type, Tokens args, std::string_view what,
                             Parsed<double> (*parse)(const Token&, std::string_view)) {
    const Parsed<double> number = parse(args[0], what);
    if (!number) {
        return std::unexpected{number.error()};
    }
    return checked(Command{.type = type, .value = *number});
}

Parsed<Command> deck_flag(CommandType type, const Token& token, bool on) {
    const Parsed<std::size_t> target = deck(token);
    if (!target) {
        return std::unexpected{target.error()};
    }
    return checked(Command{.type = type, .deck = *target, .on = on});
}

Parsed<Command> assign(Tokens args) {
    const Parsed<std::size_t> target = deck(args[0]);
    if (!target) {
        return std::unexpected{target.error()};
    }
    const Parsed<std::string_view> text = word(args[1]);
    if (!text) {
        return std::unexpected{text.error()};
    }
    engine::CrossfaderSide side = engine::CrossfaderSide::Thru;
    if (*text == "a") {
        side = engine::CrossfaderSide::A;
    } else if (*text == "b") {
        side = engine::CrossfaderSide::B;
    } else if (*text != "thru") {
        return line_error("a crossfader side must be a, b, or thru, not " + quoted(*text));
    }
    return checked(
        Command{.type = CommandType::SetCrossfaderAssign, .deck = *target, .side = side});
}

Parsed<Command> pfl(Tokens args) {
    const Parsed<std::string_view> text = word(args[1]);
    if (!text) {
        return std::unexpected{text.error()};
    }
    if (*text != "on" && *text != "off") {
        return line_error("pfl must be on or off, not " + quoted(*text));
    }
    return deck_flag(CommandType::SetChannelCue, args[0], *text == "on");
}

Parsed<Command> busmap(Tokens args) {
    const Parsed<std::size_t> main = output_channel(args[0]);
    if (!main) {
        return std::unexpected{main.error()};
    }
    const Parsed<std::size_t> cue = output_channel(args[1]);
    if (!cue) {
        return std::unexpected{cue.error()};
    }
    return checked(Command{.type = CommandType::SetBusMap,
                           .bus_map = engine::BusMap{.main = *main, .cue = *cue}});
}

/// How a timed command's arguments read.
enum class Form : std::uint8_t {
    /// `<deck>`, turning the command on.
    DeckOn,
    /// `<deck>`, turning the command off.
    DeckOff,
    /// `<deck> <value>`.
    DeckValue,
    /// `<deck> <dB>`.
    DeckDecibels,
    /// `<value>`.
    GlobalValue,
    /// `<dB>`.
    GlobalDecibels,
    /// `<deck> a|b|thru`.
    Assign,
    /// `<deck> on|off`.
    Pfl,
    /// `<main> <cue>`.
    BusMap,
};

constexpr std::size_t argument_count(Form form) noexcept {
    switch (form) {
    case Form::DeckOn:
    case Form::DeckOff:
    case Form::GlobalValue:
    case Form::GlobalDecibels:
        return 1;
    case Form::DeckValue:
    case Form::DeckDecibels:
    case Form::Assign:
    case Form::Pfl:
    case Form::BusMap:
        return 2;
    }
    return 0;
}

/// A timed command that becomes an engine::Command.
struct CommandForm {
    std::string_view keyword;
    CommandType type;
    Form form;
    /// What its value is, for error messages.
    std::string_view what;
};

constexpr std::array kCommandForms{
    CommandForm{
        .keyword = "play", .type = CommandType::SetPlaying, .form = Form::DeckOn, .what = ""},
    CommandForm{
        .keyword = "pause", .type = CommandType::SetPlaying, .form = Form::DeckOff, .what = ""},
    CommandForm{
        .keyword = "rate", .type = CommandType::SetRate, .form = Form::DeckValue, .what = "a rate"},
    CommandForm{.keyword = "trim",
                .type = CommandType::SetTrim,
                .form = Form::DeckDecibels,
                .what = "a trim"},
    CommandForm{.keyword = "fader",
                .type = CommandType::SetFader,
                .form = Form::DeckValue,
                .what = "a fader position"},
    CommandForm{.keyword = "assign",
                .type = CommandType::SetCrossfaderAssign,
                .form = Form::Assign,
                .what = ""},
    CommandForm{
        .keyword = "pfl", .type = CommandType::SetChannelCue, .form = Form::Pfl, .what = ""},
    CommandForm{.keyword = "crossfader",
                .type = CommandType::SetCrossfader,
                .form = Form::GlobalValue,
                .what = "a crossfader position"},
    CommandForm{.keyword = "curve",
                .type = CommandType::SetCrossfaderCurve,
                .form = Form::GlobalValue,
                .what = "a curve"},
    CommandForm{.keyword = "main",
                .type = CommandType::SetMainLevel,
                .form = Form::GlobalDecibels,
                .what = "a level"},
    CommandForm{.keyword = "headphones",
                .type = CommandType::SetHeadphoneLevel,
                .form = Form::GlobalDecibels,
                .what = "a level"},
    CommandForm{.keyword = "cuemix",
                .type = CommandType::SetCueMix,
                .form = Form::GlobalValue,
                .what = "a cue mix"},
    CommandForm{
        .keyword = "busmap", .type = CommandType::SetBusMap, .form = Form::BusMap, .what = ""},
};

/// Reads `args`, which has argument_count(form.form) tokens.
Parsed<Command> read_form(const CommandForm& form, Tokens args) {
    switch (form.form) {
    case Form::DeckOn:
        return deck_flag(form.type, args[0], true);
    case Form::DeckOff:
        return deck_flag(form.type, args[0], false);
    case Form::DeckValue:
        return deck_value(form.type, args, form.what, value);
    case Form::DeckDecibels:
        return deck_value(form.type, args, form.what, decibels);
    case Form::GlobalValue:
        return global_value(form.type, args, form.what, value);
    case Form::GlobalDecibels:
        return global_value(form.type, args, form.what, decibels);
    case Form::Assign:
        return assign(args);
    case Form::Pfl:
        return pfl(args);
    case Form::BusMap:
        return busmap(args);
    }
    return line_error("an unknown command form");
}

/// Whether x is in (lo, hi]. Written so that NaN fails.
constexpr bool in_upper_closed(double x, double lo, double hi) noexcept {
    return x > lo && x <= hi;
}

/// Checks that `keyword` has `expected` arguments.
Parsed<std::monostate> arity(std::string_view keyword, Tokens args, std::size_t expected) {
    if (args.size() == expected) {
        return std::monostate{};
    }
    return line_error(quoted(keyword) + " takes " + std::to_string(expected) +
                      (expected == 1 ? " argument" : " arguments") + ", not " +
                      std::to_string(args.size()));
}

/// The arguments of a synthetic source, then `rate <hz>` if it's there.
struct SignalArguments {
    Tokens own;
    std::optional<Token> rate;
};

Parsed<SignalArguments> split_rate(std::string_view kind, Tokens args, std::size_t own) {
    if (args.size() == own) {
        return SignalArguments{.own = args, .rate = std::nullopt};
    }
    if (args.size() == own + 2 && !args[own].quoted && args[own].text == "rate") {
        return SignalArguments{.own = args.first(own), .rate = args[own + 1]};
    }
    return line_error(quoted(kind) + " takes " + std::to_string(own) +
                      (own == 1 ? " argument" : " arguments") + ", then optionally rate <hz>");
}

/// Checks a frequency against the signal's sample rate.
std::optional<std::string> frequency_problem(double hz, std::uint32_t rate) {
    if (hz > 0.0 && hz < rate / 2.0) {
        return std::nullopt;
    }
    return "a frequency must be above 0 and below half the sample rate (" +
           std::to_string(rate / 2) + " Hz)";
}

Parsed<Signal> signal(std::string_view kind, Tokens args) {
    Signal result;
    std::size_t own = 0;
    if (kind == "sine") {
        result.kind = SignalKind::Sine;
        own = 2;
    } else if (kind == "sweep") {
        result.kind = SignalKind::Sweep;
        own = 3;
    } else if (kind == "noise") {
        result.kind = SignalKind::Noise;
        own = 1;
    } else if (kind == "clicks") {
        result.kind = SignalKind::Clicks;
        own = 2;
    } else {
        return line_error("a track's source must be file, sine, sweep, noise, or clicks, not " +
                          quoted(kind));
    }
    const Parsed<SignalArguments> split = split_rate(kind, args, own);
    if (!split) {
        return std::unexpected{split.error()};
    }
    if (split->rate) {
        const Parsed<std::uint64_t> rate =
            whole(*split->rate, "a sample rate", engine::kMinTrackRate, engine::kMaxTrackRate);
        if (!rate) {
            return std::unexpected{rate.error()};
        }
        result.sample_rate = static_cast<std::uint32_t>(*rate);
    }

    // Every source's last own argument is its length.
    std::vector<double> numbers;
    for (const Token& token : split->own) {
        const Parsed<double> number = amount(token, "a frequency, BPM, or length");
        if (!number) {
            return std::unexpected{number.error()};
        }
        numbers.push_back(*number);
    }
    result.seconds = numbers.back();
    if (!in_upper_closed(result.seconds, 0.0, kMaxSignalSeconds)) {
        return line_error("a signal's length must be above 0 and at most 3,600 seconds");
    }
    switch (result.kind) {
    case SignalKind::Sine:
        result.frequency_hz = numbers.front();
        break;
    case SignalKind::Sweep:
        result.frequency_hz = numbers.front();
        result.to_hz = numbers.at(1);
        break;
    case SignalKind::Noise:
        break;
    case SignalKind::Clicks:
        result.bpm = numbers.front();
        if (!in_upper_closed(result.bpm, 0.0, kMaxBpm)) {
            return line_error("clicks' BPM must be above 0 and at most 1,000");
        }
        break;
    }
    if (result.kind == SignalKind::Sine || result.kind == SignalKind::Sweep) {
        if (auto problem = frequency_problem(result.frequency_hz, result.sample_rate)) {
            return line_error(std::move(*problem));
        }
    }
    if (result.kind == SignalKind::Sweep) {
        if (auto problem = frequency_problem(result.to_hz, result.sample_rate)) {
            return line_error(std::move(*problem));
        }
    }
    return result;
}

/// A `load` whose track name is resolved once every track line is read:
/// track lines may come after the loads that name them (requirement 8).
struct PendingLoad {
    std::size_t event = 0;
    std::string_view name;
    std::size_t line = 0;
};

/// An open `<time> repeat <count> every <period>` block, until its
/// `end repeat` (requirement 12).
struct RepeatBlock {
    double time = 0.0;
    std::uint64_t count = 0;
    double period = 0.0;
    /// The `repeat` line.
    std::size_t line = 0;
    /// The lines inside, in script order, each at its offset.
    std::vector<Event> events;
    /// Loads among `events`; PendingLoad::event indexes `events`.
    std::vector<PendingLoad> loads;
};

/// Whether `tokens` is `end repeat`.
bool is_end_repeat(Tokens tokens) {
    return tokens.size() == 2 && !tokens[0].quoted && tokens[0].text == "end" &&
           !tokens[1].quoted && tokens[1].text == "repeat";
}

class Reader {
public:
    explicit Reader(std::filesystem::path directory) : directory_{std::move(directory)} {}

    std::expected<Script, ScriptError> read(std::string_view text);

private:
    /// Reads one line's tokens; an error is on that line.
    Parsed<std::monostate> read_line(Tokens tokens, std::size_t line);
    Parsed<std::monostate> read_track(Tokens tokens, std::size_t line);
    Parsed<std::monostate> read_timed(Tokens tokens, std::size_t line);
    /// A timed line's command, after its time.
    Parsed<std::monostate> read_command(double time, std::string_view keyword, Tokens args,
                                        std::size_t line);
    Parsed<std::monostate> read_end(double time, std::size_t line);
    Parsed<std::monostate> read_load(double time, Tokens args, std::size_t line);
    /// `seek` or `cuepoint`.
    Parsed<std::monostate> read_track_time(double time, CommandType type, Tokens args,
                                           std::size_t line);
    Parsed<std::monostate> read_repeat(double time, Tokens args, std::size_t line);
    /// Expands `block` at its `end repeat` on `line`. An error may be on a
    /// line inside the block.
    std::optional<ScriptError> close_block(const RepeatBlock& block, std::size_t line);
    Parsed<std::monostate> add_event(double time, const Action& action, std::size_t line);
    std::optional<ScriptError> finish();
    /// Gives each seek and cue point its deck's track, walking the events
    /// in run order (requirement 10).
    std::optional<ScriptError> resolve_track_times();

    std::filesystem::path directory_;
    Script script_;
    std::optional<std::size_t> end_line_;
    std::vector<PendingLoad> loads_;
    std::optional<RepeatBlock> block_;
};

std::expected<Script, ScriptError> Reader::read(std::string_view text) {
    if (text.size() > kMaxScriptBytes) {
        return std::unexpected{ScriptError{.line = 0, .message = "the script is over 1 MiB"}};
    }
    std::size_t line = 0;
    for (const std::string_view content : lexer::split_lines(text)) {
        ++line;
        const auto tokens = lexer::tokenize(content);
        if (!tokens) {
            return std::unexpected{ScriptError{.line = line, .message = tokens.error()}};
        }
        if (tokens->empty()) {
            continue;
        }
        if (block_ && is_end_repeat(*tokens)) {
            const RepeatBlock block = std::move(*block_);
            block_.reset();
            if (auto error = close_block(block, line)) {
                return std::unexpected{std::move(*error)};
            }
            continue;
        }
        if (const auto read = read_line(*tokens, line); !read) {
            return std::unexpected{ScriptError{.line = line, .message = read.error()}};
        }
    }
    if (block_) {
        return std::unexpected{ScriptError{.line = block_->line,
                                           .message = "this repeat block has no end repeat line"}};
    }
    if (auto error = finish()) {
        return std::unexpected{std::move(*error)};
    }
    return std::move(script_);
}

Parsed<std::monostate> Reader::read_line(Tokens tokens, std::size_t line) {
    const Token& first = tokens.front();
    if (!first.quoted && first.text == "track") {
        if (block_) {
            return line_error("a track line can't be inside a repeat block: put it before the "
                              "repeat line, on line " +
                              std::to_string(block_->line));
        }
        return read_track(tokens, line);
    }
    if (!first.quoted && first.text == "end") {
        // An open block's end repeat is handled by read().
        if (is_end_repeat(tokens)) {
            return line_error("an end repeat with no repeat before it");
        }
        return line_error("an end needs a time, as in \"60 end\"");
    }
    return read_timed(tokens, line);
}

Parsed<std::monostate> Reader::read_track(Tokens tokens, std::size_t line) {
    if (tokens.size() < 3) {
        return line_error("a track line is track <name> <source>");
    }
    const Parsed<std::string_view> name = word(tokens[1]);
    if (!name) {
        return std::unexpected{name.error()};
    }
    if (!lexer::is_track_name(*name)) {
        return line_error(quoted(*name) +
                          " isn't a track name: start with a letter or _, then use letters, "
                          "digits, _ or -");
    }
    for (const TrackSpec& track : script_.tracks) {
        if (track.name == *name) {
            return line_error("track " + quoted(*name) + " is already named on line " +
                              std::to_string(track.line));
        }
    }
    if (script_.tracks.size() >= kMaxTracks) {
        return line_error("a script can name at most 64 tracks");
    }
    const Parsed<std::string_view> kind = word(tokens[2]);
    if (!kind) {
        return std::unexpected{kind.error()};
    }
    const Tokens args = tokens.subspan(3);
    TrackSpec spec{.name = std::string{*name}, .source = TrackFile{}, .line = line};
    if (*kind == "file") {
        if (args.size() != 1 || !args[0].quoted) {
            return line_error("a file source is file \"<path>\"");
        }
        const std::filesystem::path path{args[0].text};
        spec.source = TrackFile{.path = path.is_absolute() ? path : directory_ / path};
    } else {
        Parsed<Signal> source = signal(*kind, args);
        if (!source) {
            return std::unexpected{source.error()};
        }
        spec.source = *source;
    }
    script_.tracks.push_back(std::move(spec));
    return std::monostate{};
}

Parsed<std::monostate> Reader::read_timed(Tokens tokens, std::size_t line) {
    const Parsed<double> time = amount(tokens[0], "a line's first word, its time,");
    if (!time) {
        return line_error("a line must start with track, or with a time such as 12 or 0.25, "
                          "not " +
                          quoted(tokens[0].text));
    }
    if (!(*time <= kMaxTime)) {
        return line_error("a time must be at most 86,400 seconds (24 hours)");
    }
    if (block_ && !(*time < block_->period)) {
        return line_error("a time inside a repeat block is an offset, and must be below the "
                          "block's period, on line " +
                          std::to_string(block_->line));
    }
    if (tokens.size() < 2) {
        return line_error("a time must be followed by a command");
    }
    const Parsed<std::string_view> keyword = word(tokens[1]);
    if (!keyword) {
        return std::unexpected{keyword.error()};
    }
    return read_command(*time, *keyword, tokens.subspan(2), line);
}

Parsed<std::monostate> Reader::read_command(double time, std::string_view keyword, Tokens args,
                                            std::size_t line) {
    if (keyword == "end") {
        if (auto ok = arity(keyword, args, 0); !ok) {
            return ok;
        }
        return read_end(time, line);
    }
    if (keyword == "load") {
        if (auto ok = arity(keyword, args, 2); !ok) {
            return ok;
        }
        return read_load(time, args, line);
    }
    if (keyword == "unload") {
        if (auto ok = arity(keyword, args, 1); !ok) {
            return ok;
        }
        const Parsed<std::size_t> target = deck(args[0]);
        if (!target) {
            return std::unexpected{target.error()};
        }
        return add_event(time, Unload{.deck = *target}, line);
    }
    if (keyword == "seek" || keyword == "cuepoint") {
        if (auto ok = arity(keyword, args, 2); !ok) {
            return ok;
        }
        return read_track_time(
            time, keyword == "seek" ? CommandType::Seek : CommandType::SetCuePoint, args, line);
    }
    if (keyword == "repeat") {
        return read_repeat(time, args, line);
    }
    const auto* form = std::ranges::find(kCommandForms, keyword, &CommandForm::keyword);
    if (form == kCommandForms.end()) {
        return line_error("unknown command " + quoted(keyword));
    }
    if (auto ok = arity(keyword, args, argument_count(form->form)); !ok) {
        return ok;
    }
    const Parsed<Command> command = read_form(*form, args);
    if (!command) {
        return std::unexpected{command.error()};
    }
    return add_event(time, *command, line);
}

Parsed<std::monostate> Reader::read_end(double time, std::size_t line) {
    if (block_) {
        return line_error("the end can't be inside a repeat block");
    }
    if (end_line_) {
        return line_error("a second end: the first is on line " + std::to_string(*end_line_));
    }
    if (!(time > 0.0)) {
        return line_error("the end must come after 0 seconds");
    }
    script_.end = time;
    end_line_ = line;
    return std::monostate{};
}

Parsed<std::monostate> Reader::read_load(double time, Tokens args, std::size_t line) {
    const Parsed<std::size_t> target = deck(args[0]);
    if (!target) {
        return std::unexpected{target.error()};
    }
    const Parsed<std::string_view> name = word(args[1]);
    if (!name) {
        return std::unexpected{name.error()};
    }
    if (auto added = add_event(time, Load{.deck = *target}, line); !added) {
        return added;
    }
    // Inside a block, the load is resolved once per repetition when the
    // block is expanded.
    std::vector<PendingLoad>& loads = block_ ? block_->loads : loads_;
    const std::size_t index = (block_ ? block_->events.size() : script_.events.size()) - 1;
    loads.push_back(PendingLoad{.event = index, .name = *name, .line = line});
    return std::monostate{};
}

Parsed<std::monostate> Reader::read_repeat(double time, Tokens args, std::size_t line) {
    if (block_) {
        return line_error("repeat blocks can't be nested: the block on line " +
                          std::to_string(block_->line) + " is still open");
    }
    if (auto ok = arity("repeat", args, 3); !ok) {
        return ok;
    }
    const Parsed<std::uint64_t> count = whole(args[0], "a repeat count", 1, kMaxRepeatCount);
    if (!count) {
        return std::unexpected{count.error()};
    }
    if (args[1].quoted || args[1].text != "every") {
        return line_error("a repeat line is <time> repeat <count> every <seconds>");
    }
    const Parsed<double> period = amount(args[2], "a repeat's period");
    if (!period) {
        return std::unexpected{period.error()};
    }
    if (!(*period > 0.0)) {
        return line_error("a repeat's period must be above 0 seconds");
    }
    block_ = RepeatBlock{
        .time = time, .count = *count, .period = *period, .line = line, .events = {}, .loads = {}};
    return std::monostate{};
}

std::optional<ScriptError> Reader::close_block(const RepeatBlock& block, std::size_t line) {
    // Check the count before expanding, so a huge block allocates nothing.
    // Both factors are at most 100,000, so the product can't overflow.
    const std::uint64_t expanded = block.count * block.events.size();
    if (script_.events.size() + expanded > kMaxEvents) {
        return ScriptError{.line = line,
                           .message = "this repeat block expands to " + std::to_string(expanded) +
                                      " events: a script can have at most 100,000 events, "
                                      "repeats included"};
    }
    script_.events.reserve(script_.events.size() + expanded);
    // Line by line, then repetition by repetition, so that events at the
    // same time keep the order of their lines (requirement 13).
    auto load = block.loads.begin();
    for (std::size_t inner = 0; inner < block.events.size(); ++inner) {
        const Event& event = block.events[inner];
        const bool is_load = load != block.loads.end() && load->event == inner;
        for (std::uint64_t k = 0; k < block.count; ++k) {
            const double time = block.time + (static_cast<double>(k) * block.period) + event.time;
            if (!(time <= kMaxTime)) {
                return ScriptError{.line = event.line,
                                   .message = "repetition " + std::to_string(k + 1) +
                                              " of this line is past 86,400 seconds (24 hours)"};
            }
            if (is_load) {
                loads_.push_back(PendingLoad{
                    .event = script_.events.size(), .name = load->name, .line = load->line});
            }
            script_.events.push_back(
                Event{.time = time, .action = event.action, .line = event.line});
        }
        if (is_load) {
            ++load;
        }
    }
    return std::nullopt;
}

Parsed<std::monostate> Reader::read_track_time(double time, CommandType type, Tokens args,
                                               std::size_t line) {
    const Parsed<std::size_t> target = deck(args[0]);
    if (!target) {
        return std::unexpected{target.error()};
    }
    const Parsed<double> seconds = amount(args[1], "a time in the track");
    if (!seconds) {
        return std::unexpected{seconds.error()};
    }
    // A frame at any track rate is finite at this bound, so decode() accepts
    // it whatever the rate turns out to be (plan, Decisions).
    if (!(*seconds <= kMaxTime)) {
        return line_error("a time in the track must be at most 86,400 seconds (24 hours)");
    }
    // The deck's track is filled in by finish(), once the events are in run
    // order.
    return add_event(time, TrackTime{.type = type, .deck = *target, .seconds = *seconds}, line);
}

Parsed<std::monostate> Reader::add_event(double time, const Action& action, std::size_t line) {
    if (block_) {
        // A block's lines each run at least once.
        if (script_.events.size() + block_->events.size() >= kMaxEvents) {
            return line_error("a script can have at most 100,000 events, repeats included");
        }
        block_->events.push_back(Event{.time = time, .action = action, .line = line});
        return std::monostate{};
    }
    if (script_.events.size() >= kMaxEvents) {
        return line_error("a script can have at most 100,000 events, repeats included");
    }
    script_.events.push_back(Event{.time = time, .action = action, .line = line});
    return std::monostate{};
}

std::optional<ScriptError> Reader::finish() {
    if (!end_line_) {
        return ScriptError{.line = 0, .message = "the script has no end line, such as \"60 end\""};
    }
    for (const PendingLoad& load : loads_) {
        const auto named = std::ranges::find(script_.tracks, load.name, &TrackSpec::name);
        if (named == script_.tracks.end()) {
            return ScriptError{.line = load.line,
                               .message = "no track is named " + quoted(load.name)};
        }
        std::get<Load>(script_.events.at(load.event).action).track =
            static_cast<std::size_t>(named - script_.tracks.begin());
    }
    // Events are still in script order here.
    for (const Event& event : script_.events) {
        if (!(event.time < script_.end)) {
            return ScriptError{.line = event.line,
                               .message = "every event must come before the end, on line " +
                                          std::to_string(*end_line_)};
        }
    }
    std::ranges::stable_sort(script_.events, {}, &Event::time);
    return resolve_track_times();
}

std::optional<ScriptError> Reader::resolve_track_times() {
    std::array<std::optional<std::size_t>, engine::kDecks> decks{};
    const std::span<std::optional<std::size_t>> deck_track{decks};
    for (Event& event : script_.events) {
        if (const auto* load = std::get_if<Load>(&event.action)) {
            deck_track[load->deck] = load->track;
        } else if (const auto* unload = std::get_if<Unload>(&event.action)) {
            deck_track[unload->deck].reset();
        } else if (auto* time = std::get_if<TrackTime>(&event.action)) {
            const std::optional<std::size_t> track = deck_track[time->deck];
            if (!track) {
                return ScriptError{.line = event.line,
                                   .message =
                                       "deck " + std::to_string(time->deck + 1) +
                                       " has no track then: load one at an earlier time first"};
            }
            time->track = *track;
            // A synthetic track's rate is known now, so check the command
            // decode() would see (requirement 11). A file's rate isn't known
            // until it's decoded; read_track_time() bounds its seconds.
            if (const auto* signal = std::get_if<Signal>(&script_.tracks.at(*track).source)) {
                if (const Parsed<Command> command =
                        checked(command_for(*time, signal->sample_rate));
                    !command) {
                    return ScriptError{.line = event.line, .message = command.error()};
                }
            }
        }
    }
    return std::nullopt;
}

} // namespace

std::expected<Script, ScriptError> read_script(std::string_view text,
                                               const std::filesystem::path& directory) {
    return Reader{directory}.read(text);
}

engine::Command command_for(const TrackTime& event, std::uint32_t sample_rate) noexcept {
    return Command{.type = event.type,
                   .deck = event.deck,
                   .value = event.seconds * static_cast<double>(sample_rate)};
}

} // namespace ks::harness
