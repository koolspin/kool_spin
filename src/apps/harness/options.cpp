// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/harness/options.h>

#include <ks/engine/miniaudio_backend.h>
#include <ks/harness/run.h>
#include <ks/harness/script.h>
#include <ks/harness/tracks.h>

#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
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

namespace {

/// --list-devices: the backend's output devices, numbered from 0, with the
/// default marked, as ks-tone lists them.
int list_devices(engine::MiniaudioBackends backends, std::ostream& out, std::ostream& err) {
    const auto list = engine::list_output_devices(backends);
    if (!list) {
        err << "ks-harness: the output devices couldn't be listed: " << list.error().message
            << '\n';
        return kExitRuntimeError;
    }
    out << std::format("Output devices on {}:\n", list->backend);
    for (std::size_t i = 0; const engine::OutputDevice& device : list->devices) {
        out << std::format("  {}: {}{}\n", i, device.name, device.is_default ? " (default)" : "");
        ++i;
    }
    return kExitOk;
}

/// The script file's text, at most kMaxScriptBytes + 1 bytes of it: enough
/// for read_script() to refuse a script that's too long without reading all
/// of a huge file.
std::expected<std::string, std::string> read_text(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return std::unexpected{
            std::format("ks-harness: {}: the script couldn't be opened", path.string())};
    }
    std::string text(kMaxScriptBytes + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (file.bad()) {
        return std::unexpected{
            std::format("ks-harness: {}: the script couldn't be read", path.string())};
    }
    text.resize(static_cast<std::size_t>(file.gcount()));
    return text;
}

/// Device mode's options from the command line, with --device turned into the
/// device's key. On failure, returns the exit code, having printed why.
std::expected<DeviceOptions, int> device_options(const Options& options, std::ostream& err) {
    DeviceOptions device{
        .backends = options.backend.value_or(engine::MiniaudioBackends::System),
        .device = std::nullopt,
        .sample_rate = options.rate.value_or(0),
        .channels = options.channels.value_or(0),
        .period_frames = options.period.value_or(0),
    };
    if (options.device) {
        const auto list = engine::list_output_devices(device.backends);
        if (!list) {
            err << "ks-harness: the output devices couldn't be listed: " << list.error().message
                << '\n';
            return std::unexpected{kExitRuntimeError};
        }
        if (*options.device >= list->devices.size()) {
            err << std::format("ks-harness: device {} isn't listed; --list-devices shows the "
                               "{} device numbers\n",
                               *options.device, list->devices.size());
            return std::unexpected{kExitUsageError};
        }
        device.device = std::span{list->devices}[*options.device].key;
    }
    return device;
}

} // namespace

int run(const Options& options, std::ostream& out, std::ostream& err,
        const std::atomic<bool>& stop) {
    if (options.help) {
        out << usage();
        return kExitOk;
    }
    if (options.list_devices) {
        return list_devices(options.backend.value_or(engine::MiniaudioBackends::System), out, err);
    }
    if (!options.script) {
        err << "ks-harness: no script given\n\n" << usage();
        return kExitUsageError;
    }

    // Check the device number before reading anything, as a usage error.
    std::optional<DeviceOptions> device;
    if (!options.out && !options.check) {
        auto resolved = device_options(options, err);
        if (!resolved) {
            return resolved.error();
        }
        device = *resolved;
    }

    const std::filesystem::path& path = *options.script;
    const auto text = read_text(path);
    if (!text) {
        err << text.error() << '\n';
        return kExitRuntimeError;
    }
    const auto script = read_script(*text, path.parent_path());
    if (!script) {
        err << std::format("{}:{}: {}\n", path.string(), script.error().line,
                           script.error().message);
        return kExitUsageError;
    }
    const auto tracks = TrackSet::prepare(*script);
    if (!tracks) {
        err << "ks-harness: " << tracks.error() << '\n';
        return kExitRuntimeError;
    }

    if (options.check) {
        out << std::format("{}: {} tracks, {} events, ends at {:.3f} s\n", path.string(),
                           script->tracks.size(), script->events.size(), script->end);
        return kExitOk;
    }

    std::expected<Report, std::string> report;
    if (options.out) {
        const FileOptions file{
            .path = *options.out,
            .sample_rate = options.rate.value_or(kDefaultFileRate),
            .channels = options.channels.value_or(kDefaultFileChannels),
            .block_frames = options.period.value_or(kDefaultFileBlock),
        };
        report = render_to_file(*script, *tracks, file, stop);
    } else {
        report = play_on_device(*script, *tracks, *device, stop, out);
    }
    if (!report) {
        err << "ks-harness: " << report.error() << '\n';
        return kExitRuntimeError;
    }
    print_report(*report, out);
    return flags_problem(*report) ? kExitProblem : kExitOk;
}

} // namespace ks::harness
