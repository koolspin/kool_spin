// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine_app/options.h>

#include <ks/engine/miniaudio_backend.h>

#include <charconv>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ks::engine_app {

namespace {

/// Parses a whole number made only of the digits 0-9: no sign, no spaces.
template <typename T>
std::optional<T> parse_whole(std::string_view text) noexcept {
    const char* const first = std::to_address(text.begin());
    const char* const last = std::to_address(text.end());
    if (text.empty() || text.front() < '0' || text.front() > '9') {
        return std::nullopt;
    }
    T value{};
    const auto [end, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || end != last) {
        return std::nullopt;
    }
    return value;
}

/// Sets `field` to `value`, refusing an option given twice.
template <typename T>
std::expected<void, std::string> set_once(std::optional<T>& field, std::string_view option,
                                          T value) {
    if (field) {
        return std::unexpected{std::format("ks-engine: {} given twice", option)};
    }
    field = std::move(value);
    return {};
}

/// A whole number in [min, max] for `option`.
std::expected<std::uint32_t, std::string> parse_setting(std::string_view option,
                                                        std::string_view text, std::uint32_t min,
                                                        std::uint32_t max) {
    const auto value = parse_whole<std::uint32_t>(text);
    if (!value || *value < min || *value > max) {
        return std::unexpected{
            std::format("ks-engine: {} must be a whole number from {} to {}", option, min, max)};
    }
    return *value;
}

/// A descriptor number for `option`.
std::expected<int, std::string> parse_fd(std::string_view option, std::string_view text) {
    const auto value = parse_whole<int>(text);
    if (!value) {
        return std::unexpected{std::format("ks-engine: {} must be a descriptor number", option)};
    }
    return *value;
}

bool takes_value(std::string_view arg) noexcept {
    return arg == "--supervisor-fd" || arg == "--state-fd" || arg == "--log-fd" ||
           arg == "--record-fd" || arg == "--track" || arg == "--start-delay" ||
           arg == "--runtime-dir" || arg == "--backend" || arg == "--device" || arg == "--rate" ||
           arg == "--channels" || arg == "--period";
}

/// `--track <id>:<fd>`: adds the track, refusing an ID of 0, a repeated ID or
/// descriptor, and more than kMaxTrackOptions.
std::expected<void, std::string> add_track(Options& options, std::string_view value) {
    const std::size_t colon = value.find(':');
    const std::optional<std::uint64_t> id =
        colon == std::string_view::npos ? std::nullopt
                                        : parse_whole<std::uint64_t>(value.substr(0, colon));
    const std::optional<int> fd =
        colon == std::string_view::npos ? std::nullopt : parse_whole<int>(value.substr(colon + 1));
    if (!id || !fd) {
        return std::unexpected{
            std::format("ks-engine: --track must be <id>:<descriptor>, not '{}'", value)};
    }
    if (*id == 0) {
        return std::unexpected{std::string{"ks-engine: --track: a track's ID is never 0"}};
    }
    if (options.tracks.size() >= kMaxTrackOptions) {
        return std::unexpected{
            std::format("ks-engine: --track given more than {} times", kMaxTrackOptions)};
    }
    for (const TrackOption& given : options.tracks) {
        if (given.id == *id) {
            return std::unexpected{std::format("ks-engine: --track: track {} given twice", *id)};
        }
        if (given.fd == *fd) {
            return std::unexpected{
                std::format("ks-engine: --track: descriptor {} given twice", *fd)};
        }
    }
    options.tracks.push_back(TrackOption{.id = *id, .fd = *fd});
    return {};
}

/// The output options, one of which `arg` is.
std::expected<void, std::string> apply_output(Options& options, std::string_view arg,
                                              std::string_view value) {
    if (arg == "--backend" && value == "paced") {
        if (options.paced || options.backend) {
            return std::unexpected{std::string{"ks-engine: --backend given twice"}};
        }
        options.paced = true;
        return {};
    }
    if (arg == "--backend") {
        if (options.paced) {
            return std::unexpected{std::string{"ks-engine: --backend given twice"}};
        }
        const std::optional<engine::MiniaudioBackends> backend = engine::parse_backend(value);
        if (!backend) {
            return std::unexpected{std::format("ks-engine: unknown backend '{}'", value)};
        }
        return set_once(options.backend, arg, *backend);
    }
    if (arg == "--device") {
        const std::optional<std::size_t> device = parse_whole<std::size_t>(value);
        if (!device) {
            return std::unexpected{std::string{
                "ks-engine: --device must be a device number shown by ks-harness --list-devices"}};
        }
        return set_once(options.device, arg, *device);
    }
    std::uint32_t min = kMinPeriod;
    std::uint32_t max = kMaxPeriod;
    std::optional<std::uint32_t>* field = &options.period;
    if (arg == "--rate") {
        min = kMinRate;
        max = kMaxRate;
        field = &options.rate;
    } else if (arg == "--channels") {
        min = kMinChannels;
        max = kMaxChannels;
        field = &options.channels;
    }
    const std::expected<std::uint32_t, std::string> setting = parse_setting(arg, value, min, max);
    if (!setting) {
        return std::unexpected{setting.error()};
    }
    return set_once(*field, arg, *setting);
}

/// Applies `value` to the option `arg`, one of those takes_value() accepts.
std::expected<void, std::string> apply_value(Options& options, std::string_view arg,
                                             std::string_view value) {
    if (arg == "--track") {
        return add_track(options, value);
    }
    if (arg == "--start-delay") {
        // A delay of a minute is already far longer than any test needs.
        const std::expected<std::uint32_t, std::string> delay =
            parse_setting(arg, value, 0, kMaxStartDelayMs);
        if (!delay) {
            return std::unexpected{delay.error()};
        }
        return set_once(options.start_delay_ms, arg, *delay);
    }
    if (arg == "--supervisor-fd" || arg == "--state-fd" || arg == "--log-fd" ||
        arg == "--record-fd") {
        const std::expected<int, std::string> fd = parse_fd(arg, value);
        if (!fd) {
            return std::unexpected{fd.error()};
        }
        if (arg == "--state-fd") {
            return set_once(options.state_fd, arg, *fd);
        }
        if (arg == "--log-fd") {
            return set_once(options.log_fd, arg, *fd);
        }
        if (arg == "--record-fd") {
            return set_once(options.record_fd, arg, *fd);
        }
        return set_once(options.supervisor_fd, arg, *fd);
    }
    if (arg == "--runtime-dir") {
        if (value.empty()) {
            return std::unexpected{std::string{"ks-engine: --runtime-dir needs a path"}};
        }
        return set_once(options.runtime_dir, arg, std::filesystem::path{value});
    }
    return apply_output(options, arg, value);
}

} // namespace

std::string usage() {
    return std::format(
        "Usage: ks-engine --supervisor-fd <n> --state-fd <n> --runtime-dir <path> [output "
        "options]\n"
        "\n"
        "The engine process. kool_spin starts it and gives it the first three\n"
        "options, and --log-fd <n>, its log segment; the output options come\n"
        "from after kool_spin's --. Without --log-fd, it reports to standard\n"
        "error.\n"
        "\n"
        "Output options:\n"
        "  --backend <name>   the audio backend: system (default), null, pulseaudio,\n"
        "                     alsa, jack, or coreaudio\n"
        "  --device <n>       a device number from ks-harness --list-devices\n"
        "                     (default: the default device)\n"
        "  --rate <hz>        the sample rate, {} to {} (default: the device's)\n"
        "  --channels <n>     output channels, {} to {} (default: the device's)\n"
        "  --period <frames>  the device period, {} to {} (default: the backend's)\n"
        "  --help             print this text and exit\n"
        "\n"
        "kool_spin gives a restarted engine these too:\n"
        "  --track <id>:<fd>  a track the old engine held, in the segment at <fd>;\n"
        "                     at most {}, never a repeated ID or descriptor\n"
        "  --clean            start as at launch: ignore the resume state and --track\n"
        "  --record-fd <n>    the recording segment, for --backend paced\n"
        "\n"
        "For tests: --backend paced renders in real time on a thread instead of a\n"
        "device (with --record-fd, it records), and --start-delay <ms>, which only it\n"
        "accepts, waits that long before building the engine.\n"
        "\n"
        "Exit codes: 0 stopped, 1 couldn't start, 2 usage error.\n",
        kMinRate, kMaxRate, kMinChannels, kMaxChannels, kMinPeriod, kMaxPeriod, kMaxTrackOptions);
}

std::expected<Options, std::string> parse_options(std::span<const std::string_view> args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            options.help = true;
            return options;
        }
        if (arg == "--clean") {
            if (options.clean) {
                return std::unexpected{std::string{"ks-engine: --clean given twice"}};
            }
            options.clean = true;
            continue;
        }
        if (!takes_value(arg)) {
            return std::unexpected{std::format("ks-engine: unexpected argument '{}'", arg)};
        }
        if (i + 1 >= args.size()) {
            return std::unexpected{std::format("ks-engine: {} needs a value", arg)};
        }
        if (auto applied = apply_value(options, arg, args[++i]); !applied) {
            return std::unexpected{std::move(applied.error())};
        }
    }
    if (!options.supervisor_fd) {
        return std::unexpected{std::string{"ks-engine: --supervisor-fd is required"}};
    }
    if (!options.state_fd) {
        return std::unexpected{std::string{"ks-engine: --state-fd is required"}};
    }
    if (!options.runtime_dir) {
        return std::unexpected{std::string{"ks-engine: --runtime-dir is required"}};
    }
    if (options.start_delay_ms && !options.paced) {
        return std::unexpected{std::string{"ks-engine: --start-delay is for --backend paced only"}};
    }
    if (options.record_fd && !options.paced) {
        return std::unexpected{std::string{"ks-engine: --record-fd is for --backend paced only"}};
    }
    if (options.paced && options.device) {
        return std::unexpected{std::string{"ks-engine: --backend paced has no devices"}};
    }
    return options;
}

} // namespace ks::engine_app
