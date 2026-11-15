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
    return arg == "--supervisor-fd" || arg == "--state-fd" || arg == "--runtime-dir" ||
           arg == "--backend" || arg == "--device" || arg == "--rate" || arg == "--channels" ||
           arg == "--period";
}

/// The output options, one of which `arg` is.
std::expected<void, std::string> apply_output(Options& options, std::string_view arg,
                                              std::string_view value) {
    if (arg == "--backend") {
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
    if (arg == "--supervisor-fd" || arg == "--state-fd") {
        const std::expected<int, std::string> fd = parse_fd(arg, value);
        if (!fd) {
            return std::unexpected{fd.error()};
        }
        return set_once(arg == "--state-fd" ? options.state_fd : options.supervisor_fd, arg, *fd);
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
        "options; the output options come from after kool_spin's --.\n"
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
        "Exit codes: 0 stopped, 1 couldn't start, 2 usage error.\n",
        kMinRate, kMaxRate, kMinChannels, kMaxChannels, kMinPeriod, kMaxPeriod);
}

std::expected<Options, std::string> parse_options(std::span<const std::string_view> args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            options.help = true;
            return options;
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
    return options;
}

} // namespace ks::engine_app
