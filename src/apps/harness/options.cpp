// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/harness/options.h>

#include <ks/engine/miniaudio_backend.h>

#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ks::harness {
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

/// Parses --rate, --channels, or --period: a whole number in [min, max], or
/// 0 (checked against the mode once every argument has been read).
std::expected<std::uint32_t, std::string> parse_setting(std::string_view option,
                                                        std::string_view text, std::uint32_t min,
                                                        std::uint32_t max) {
    const auto value = parse_whole<std::uint32_t>(text);
    if (!value || (*value != 0 && (*value < min || *value > max))) {
        return std::unexpected{
            std::format("ks-harness: {} must be a whole number from {} to {}, or 0 without --out",
                        option, min, max)};
    }
    return *value;
}

bool takes_value(std::string_view arg) noexcept {
    return arg == "--out" || arg == "--backend" || arg == "--device" || arg == "--rate" ||
           arg == "--channels" || arg == "--period";
}

/// Applies `value` to the option `arg`, one of those takes_value() accepts.
std::expected<void, std::string> apply_value(Options& options, std::string_view arg,
                                             std::string_view value) {
    const auto set = [](std::optional<std::uint32_t>& field) {
        return [&field](std::uint32_t v) { field = v; };
    };
    if (arg == "--out") {
        if (value.empty()) {
            return std::unexpected{std::string{"ks-harness: --out needs a file name"}};
        }
        options.out = std::filesystem::path{value};
        return {};
    }
    if (arg == "--backend") {
        options.backend = engine::parse_backend(value);
        if (!options.backend) {
            return std::unexpected{std::format("ks-harness: unknown backend '{}'", value)};
        }
        return {};
    }
    if (arg == "--device") {
        options.device = parse_whole<std::size_t>(value);
        if (!options.device) {
            return std::unexpected{std::string{
                "ks-harness: --device must be a device number shown by --list-devices"}};
        }
        return {};
    }
    if (arg == "--rate") {
        return parse_setting(arg, value, kMinRate, kMaxRate).transform(set(options.rate));
    }
    if (arg == "--channels") {
        return parse_setting(arg, value, kMinChannels, kMaxChannels)
            .transform(set(options.channels));
    }
    return parse_setting(arg, value, kMinPeriod, kMaxPeriod).transform(set(options.period));
}

/// The checks that depend on more than one option.
std::expected<void, std::string> check_combination(const Options& options) {
    if (options.out) {
        if (options.backend) {
            return std::unexpected{std::string{"ks-harness: --backend can't be used with --out"}};
        }
        if (options.device) {
            return std::unexpected{std::string{"ks-harness: --device can't be used with --out"}};
        }
        const auto zero = [](std::string_view name) {
            return std::unexpected{std::format("ks-harness: {} can't be 0 with --out", name)};
        };
        if (options.rate == 0U) {
            return zero("--rate");
        }
        if (options.channels == 0U) {
            return zero("--channels");
        }
        if (options.period == 0U) {
            return zero("--period");
        }
    }
    if (!options.script && !options.list_devices) {
        return std::unexpected{std::string{"ks-harness: no script given"}};
    }
    return {};
}

} // namespace

std::string usage() {
    return std::format(
        "Usage: ks-harness [options] <script>\n"
        "\n"
        "Plays a script of engine commands through an output device, or renders\n"
        "it into a WAV file.\n"
        "\n"
        "Options:\n"
        "  --out <file>       render into a 32-bit float WAV file instead of\n"
        "                     playing through a device\n"
        "  --backend <name>   the audio backend (default: system); not with --out\n"
        "  --device <n>       a device number from --list-devices (default: the\n"
        "                     default device); not with --out\n"
        "  --list-devices     list the backend's output devices and exit\n"
        "  --rate <hz>        the output sample rate, {} to {}\n"
        "                     (file mode default: {}; device mode: 0, the device's)\n"
        "  --channels <n>     output channels, {} to {}\n"
        "                     (file mode default: {}; device mode: 0, the device's)\n"
        "  --period <frames>  the device period, or the file mode block size, {} to {}\n"
        "                     (file mode default: {}; device mode: 0, the device's)\n"
        "  --check            read the script and prepare its tracks, print a\n"
        "                     summary, and exit\n"
        "  --help             print this text and exit\n"
        "\n"
        "Exit codes: 0 success, 1 runtime error, 2 usage or script error,\n"
        "3 the report flags a problem.\n",
        kMinRate, kMaxRate, kDefaultFileRate, kMinChannels, kMaxChannels, kDefaultFileChannels,
        kMinPeriod, kMaxPeriod, kDefaultFileBlock);
}

std::expected<Options, std::string> parse_options(std::span<const std::string_view> args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            options.help = true;
            return options;
        }
        if (arg == "--list-devices") {
            options.list_devices = true;
            continue;
        }
        if (arg == "--check") {
            options.check = true;
            continue;
        }
        if (takes_value(arg)) {
            if (i + 1 >= args.size()) {
                return std::unexpected{std::format("ks-harness: {} needs a value", arg)};
            }
            if (auto applied = apply_value(options, arg, args[++i]); !applied) {
                return std::unexpected{std::move(applied.error())};
            }
            continue;
        }
        if (arg.empty() || arg.starts_with('-')) {
            return std::unexpected{std::format("ks-harness: unexpected argument '{}'", arg)};
        }
        if (options.script) {
            return std::unexpected{std::format("ks-harness: more than one script: '{}'", arg)};
        }
        options.script = std::filesystem::path{arg};
    }
    if (auto checked = check_combination(options); !checked) {
        return std::unexpected{std::move(checked.error())};
    }
    return options;
}

// Partly a stub (specs/harness/spec.md requirement 3): only the device
// number check is in place; task 12 does the rest.
int run(const Options& options, std::ostream& /*out*/, std::ostream& err,
        const std::atomic<bool>& /*stop*/) {
    if (options.device && !options.out) {
        const auto list = engine::list_output_devices(
            options.backend.value_or(engine::MiniaudioBackends::System));
        if (!list) {
            err << "ks-harness: the output devices couldn't be listed: " << list.error().message
                << '\n';
            return kExitRuntimeError;
        }
        if (*options.device >= list->devices.size()) {
            err << std::format("ks-harness: device {} isn't listed; --list-devices shows the "
                               "{} device numbers\n",
                               *options.device, list->devices.size());
            return kExitUsageError;
        }
    }
    return kExitRuntimeError;
}

} // namespace ks::harness
