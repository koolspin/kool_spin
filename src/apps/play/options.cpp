// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/play_app/options.h>

#include <ks/client/session.h>
#include <ks/ipc/state.h>

#include "numbers.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ks::play_app {
namespace {

using Parsed = std::expected<Invocation, std::string>;

constexpr std::uint32_t kMinIntervalMs = 50;
constexpr std::uint32_t kMaxIntervalMs = 60'000;

/// The arguments still to be read.
class Arguments {
public:
    explicit Arguments(std::span<const std::string_view> args) noexcept : args_{args} {}

    [[nodiscard]] bool empty() const noexcept { return next_ >= args_.size(); }

    [[nodiscard]] std::string_view peek() const noexcept { return args_[next_]; }

    std::optional<std::string_view> take() noexcept {
        if (empty()) {
            return std::nullopt;
        }
        return args_[next_++];
    }

private:
    std::span<const std::string_view> args_;
    std::size_t next_ = 0;
};

std::unexpected<std::string> fail(std::string reason) {
    return std::unexpected{std::move(reason)};
}

/// The command's next argument, or why not.
std::expected<std::string_view, std::string> required(Arguments& args, std::string_view command,
                                                      std::string_view what) {
    if (const std::optional<std::string_view> arg = args.take()) {
        return *arg;
    }
    return fail(std::format("{}: missing {}", command, what));
}

/// `<deck>` or `<channel>`: 0 to 3.
std::expected<std::uint32_t, std::string> required_index(Arguments& args, std::string_view command,
                                                         std::string_view what) {
    const auto text = required(args, command, what);
    if (!text) {
        return std::unexpected{text.error()};
    }
    const std::optional<std::uint32_t> value = parse_index(*text);
    if (!value) {
        return fail(std::format("{}: {} must be 0 to {}, not '{}'", command, what,
                                ipc::kStateDecks - 1, *text));
    }
    return *value;
}

/// Every argument has been read.
std::expected<void, std::string> no_more(const Arguments& args, std::string_view command) {
    if (!args.empty()) {
        return fail(std::format("{}: unexpected argument '{}'", command, args.peek()));
    }
    return {};
}

std::expected<Request, std::string> parse_load(Arguments& args, const std::filesystem::path& cwd) {
    const auto deck = required_index(args, "load", "deck");
    if (!deck) {
        return std::unexpected{deck.error()};
    }
    const auto path = required(args, "load", "path");
    if (!path) {
        return std::unexpected{path.error()};
    }
    if (path->empty()) {
        return fail("load: the path is empty");
    }
    const std::filesystem::path given{*path};
    Load load{.deck = *deck,
              .path = (given.is_absolute() ? given : cwd / given).lexically_normal().string(),
              .wait = false};
    if (!args.empty() && args.peek() == "--wait") {
        load.wait = true;
        static_cast<void>(args.take());
    }
    if (const auto done = no_more(args, "load"); !done) {
        return std::unexpected{done.error()};
    }
    return load;
}

std::expected<Request, std::string> parse_unload(Arguments& args) {
    const auto deck = required_index(args, "unload", "deck");
    if (!deck) {
        return std::unexpected{deck.error()};
    }
    if (const auto done = no_more(args, "unload"); !done) {
        return std::unexpected{done.error()};
    }
    return Unload{.deck = *deck};
}

std::expected<Request, std::string> parse_status(Arguments& args) {
    Status status;
    bool seen_watch = false;
    bool seen_interval = false;
    while (const std::optional<std::string_view> arg = args.take()) {
        if (*arg == "--watch" && !seen_watch) {
            seen_watch = true;
            status.watch = true;
        } else if (*arg == "--interval" && !seen_interval) {
            seen_interval = true;
            const auto text = required(args, "status", "--interval's milliseconds");
            if (!text) {
                return std::unexpected{text.error()};
            }
            const std::optional<std::uint64_t> value = parse_whole(*text);
            if (!value || *value < kMinIntervalMs || *value > kMaxIntervalMs) {
                return fail(
                    std::format("status: --interval must be {} to {} milliseconds, not '{}'",
                                kMinIntervalMs, kMaxIntervalMs, *text));
            }
            status.interval_ms = static_cast<std::uint32_t>(*value);
        } else {
            return fail(std::format("status: unexpected argument '{}'", *arg));
        }
    }
    return status;
}

/// `<real>`, or `-inf` where the command allows it.
std::expected<double, std::string> required_real(Arguments& args, std::string_view command,
                                                 std::string_view what,
                                                 NegativeInfinity negative_infinity) {
    const auto text = required(args, command, what);
    if (!text) {
        return std::unexpected{text.error()};
    }
    const std::optional<double> value = parse_real(*text, negative_infinity);
    if (!value) {
        return fail(std::format("{}: {} must be a decimal number{}, not '{}'", command, what,
                                negative_infinity == NegativeInfinity::Allowed ? " or -inf" : "",
                                *text));
    }
    return *value;
}

/// A whole number that fits in std::uint32_t.
std::expected<std::uint32_t, std::string> required_u32(Arguments& args, std::string_view command,
                                                       std::string_view what) {
    const auto text = required(args, command, what);
    if (!text) {
        return std::unexpected{text.error()};
    }
    const std::optional<std::uint64_t> value = parse_whole(*text);
    if (!value || *value > std::numeric_limits<std::uint32_t>::max()) {
        return fail(std::format("{}: {} must be a whole number from 0 to {}, not '{}'", command,
                                what, std::numeric_limits<std::uint32_t>::max(), *text));
    }
    return static_cast<std::uint32_t>(*value);
}

/// One of `words`, as its index.
std::expected<std::uint32_t, std::string> required_word(Arguments& args, std::string_view command,
                                                        std::string_view what,
                                                        std::span<const std::string_view> words) {
    const auto text = required(args, command, what);
    if (!text) {
        return std::unexpected{text.error()};
    }
    std::uint32_t index = 0;
    std::string choices;
    for (const std::string_view word : words) {
        if (*text == word) {
            return index;
        }
        choices += choices.empty() ? "" : "|";
        choices += word;
        ++index;
    }
    return fail(std::format("{}: {} must be {}, not '{}'", command, what, choices, *text));
}

/// Finishes a control command: no arguments may be left.
std::expected<Request, std::string> control(const Arguments& args, std::string_view command,
                                            const client::Command& built) {
    if (const auto done = no_more(args, command); !done) {
        return std::unexpected{done.error()};
    }
    return Control{.command = built};
}

/// `<deck> <real>` or `<channel> <real>`.
std::expected<Request, std::string>
parse_index_real(Arguments& args, std::string_view command, std::string_view index_what,
                 std::string_view real_what, NegativeInfinity negative_infinity,
                 client::Command (*build)(std::uint32_t, double) noexcept) {
    const auto index = required_index(args, command, index_what);
    if (!index) {
        return std::unexpected{index.error()};
    }
    const auto value = required_real(args, command, real_what, negative_infinity);
    if (!value) {
        return std::unexpected{value.error()};
    }
    return control(args, command, build(*index, *value));
}

/// A mixer command: `<real>`.
std::expected<Request, std::string> parse_mixer_real(Arguments& args, std::string_view command,
                                                     std::string_view what,
                                                     NegativeInfinity negative_infinity,
                                                     client::Command (*build)(double) noexcept) {
    const auto value = required_real(args, command, what, negative_infinity);
    if (!value) {
        return std::unexpected{value.error()};
    }
    return control(args, command, build(*value));
}

std::expected<Request, std::string> parse_playing(Arguments& args, std::string_view command,
                                                  bool on) {
    const auto deck = required_index(args, command, "deck");
    if (!deck) {
        return std::unexpected{deck.error()};
    }
    return control(args, command, client::set_playing(*deck, on));
}

std::expected<Request, std::string> parse_assign(Arguments& args) {
    constexpr std::array<std::string_view, 3> kSides{"a", "b", "thru"};
    const auto channel = required_index(args, "assign", "channel");
    if (!channel) {
        return std::unexpected{channel.error()};
    }
    const auto side = required_word(args, "assign", "side", kSides);
    if (!side) {
        return std::unexpected{side.error()};
    }
    return control(args, "assign", client::set_crossfader_assign(*channel, *side));
}

std::expected<Request, std::string> parse_channel_cue(Arguments& args) {
    constexpr std::array<std::string_view, 2> kStates{"off", "on"};
    const auto channel = required_index(args, "channel-cue", "channel");
    if (!channel) {
        return std::unexpected{channel.error()};
    }
    const auto state = required_word(args, "channel-cue", "the cue", kStates);
    if (!state) {
        return std::unexpected{state.error()};
    }
    return control(args, "channel-cue", client::set_channel_cue(*channel, *state == 1));
}

std::expected<Request, std::string> parse_bus_map(Arguments& args) {
    const auto main = required_u32(args, "bus-map", "the main channel");
    if (!main) {
        return std::unexpected{main.error()};
    }
    const auto cue = required_u32(args, "bus-map", "the cue channel");
    if (!cue) {
        return std::unexpected{cue.error()};
    }
    return control(args, "bus-map", client::set_bus_map(*main, *cue));
}

/// A control command, or nothing if `command` isn't one.
std::optional<std::expected<Request, std::string>> parse_control(std::string_view command,
                                                                 Arguments& args) {
    using enum NegativeInfinity;
    if (command == "play" || command == "pause") {
        return parse_playing(args, command, command == "play");
    }
    if (command == "rate") {
        return parse_index_real(args, command, "deck", "rate", Refused, client::set_rate);
    }
    if (command == "seek") {
        return parse_index_real(args, command, "deck", "frame", Refused, client::seek);
    }
    if (command == "cue-point") {
        return parse_index_real(args, command, "deck", "frame", Refused, client::set_cue_point);
    }
    if (command == "trim") {
        return parse_index_real(args, command, "channel", "dB", Allowed, client::set_trim);
    }
    if (command == "fader") {
        return parse_index_real(args, command, "channel", "position", Refused, client::set_fader);
    }
    if (command == "assign") {
        return parse_assign(args);
    }
    if (command == "channel-cue") {
        return parse_channel_cue(args);
    }
    if (command == "crossfader") {
        return parse_mixer_real(args, command, "position", Refused, client::set_crossfader);
    }
    if (command == "curve") {
        return parse_mixer_real(args, command, "transition", Refused, client::set_crossfader_curve);
    }
    if (command == "main-level") {
        return parse_mixer_real(args, command, "dB", Allowed, client::set_main_level);
    }
    if (command == "headphone-level") {
        return parse_mixer_real(args, command, "dB", Allowed, client::set_headphone_level);
    }
    if (command == "cue-mix") {
        return parse_mixer_real(args, command, "position", Refused, client::set_cue_mix);
    }
    if (command == "bus-map") {
        return parse_bus_map(args);
    }
    return std::nullopt;
}

/// A command without arguments.
template <typename Command>
std::expected<Request, std::string> parse_bare(Arguments& args, std::string_view command) {
    if (const auto done = no_more(args, command); !done) {
        return std::unexpected{done.error()};
    }
    return Command{};
}

std::expected<Request, std::string> parse_command(std::string_view command, Arguments& args,
                                                  const std::filesystem::path& cwd) {
    if (command == "load") {
        return parse_load(args, cwd);
    }
    if (command == "unload") {
        return parse_unload(args);
    }
    if (command == "status") {
        return parse_status(args);
    }
    if (command == "stop") {
        return parse_bare<Stop>(args, command);
    }
    if (command == "ping") {
        return parse_bare<Ping>(args, command);
    }
    if (auto parsed = parse_control(command, args)) {
        return std::move(*parsed);
    }
    return fail(std::format("unknown command '{}'", command));
}

} // namespace

std::expected<Invocation, std::string> parse(std::span<const std::string_view> args_in,
                                             const std::filesystem::path& cwd,
                                             const std::filesystem::path& default_runtime_dir) {
    Arguments args{args_in};
    Invocation invocation;
    std::optional<std::filesystem::path> runtime_dir;
    bool seen_name = false;
    while (!args.empty() && args.peek().starts_with("--")) {
        const std::string_view option = args.take().value_or(std::string_view{});
        if (option == "--help") {
            invocation.request = Help{};
            invocation.runtime_dir = runtime_dir.value_or(default_runtime_dir);
            return invocation;
        }
        if (option == "--runtime-dir") {
            if (runtime_dir) {
                return fail("--runtime-dir given twice");
            }
            const std::optional<std::string_view> dir = args.take();
            if (!dir || dir->empty()) {
                return fail("--runtime-dir needs a directory");
            }
            runtime_dir = std::filesystem::path{*dir};
        } else if (option == "--name") {
            if (seen_name) {
                return fail("--name given twice");
            }
            seen_name = true;
            const std::optional<std::string_view> name = args.take();
            if (!name || name->empty() || name->size() > client::kMaxNameSize) {
                return fail(
                    std::format("--name needs a name of 1 to {} bytes", client::kMaxNameSize));
            }
            invocation.name = std::string{*name};
        } else {
            return fail(std::format("unknown option '{}'", option));
        }
    }
    const std::optional<std::string_view> command = args.take();
    if (!command) {
        return fail("no command");
    }
    auto request = parse_command(*command, args, cwd);
    if (!request) {
        return std::unexpected{std::move(request.error())};
    }
    invocation.request = std::move(*request);
    invocation.runtime_dir = runtime_dir.value_or(default_runtime_dir);
    return invocation;
}

std::string usage() {
    return "usage: ks-play [--runtime-dir <dir>] [--name <name>] <command> [arguments]\n"
           "       ks-play --help\n"
           "\n"
           "Talks to a running kool_spin.\n"
           "\n"
           "Options:\n"
           "  --runtime-dir <dir>   the directory that holds supervisor.sock\n"
           "                        (default: the one kool_spin uses)\n"
           "  --name <name>         the name kool_spin knows this client by, 1 to 64 bytes\n"
           "                        (default: ks-play)\n"
           "  --help                print this and exit\n"
           "\n"
           "Commands (decks and channels are 0 to 3):\n"
           "  load <deck> <path> [--wait]   load a track; --wait waits for its decoding\n"
           "  unload <deck>                 empty a deck\n"
           "  play <deck>, pause <deck>     start or stop a deck\n"
           "  rate <deck> <rate>            set a deck's rate (1 is normal speed)\n"
           "  seek <deck> <frame>           move a deck's playhead\n"
           "  cue-point <deck> <frame>      set a deck's cue point\n"
           "  trim <channel> <db>           set a channel's trim (-inf allowed)\n"
           "  fader <channel> <position>    set a channel's fader\n"
           "  assign <channel> a|b|thru     set a channel's crossfader side\n"
           "  channel-cue <channel> on|off  send a channel to the headphones\n"
           "  crossfader <position>         set the crossfader\n"
           "  curve <transition>            set the crossfader's curve\n"
           "  main-level <db>               set the main level (-inf allowed)\n"
           "  headphone-level <db>          set the headphone level (-inf allowed)\n"
           "  cue-mix <position>            set the headphones' cue mix\n"
           "  bus-map <main> <cue>          set the main and cue outputs' channels\n"
           "  status [--watch] [--interval <ms>]\n"
           "                                show the engine's state; --watch repeats it\n"
           "                                every interval (50 to 60000 ms, default 500)\n"
           "  stop                          fade out and stop kool_spin\n"
           "  ping                          check that kool_spin answers\n"
           "\n"
           "Exit status: 0 success, 1 failure, 2 usage error, 3 kool_spin not reachable.\n";
}

} // namespace ks::play_app
