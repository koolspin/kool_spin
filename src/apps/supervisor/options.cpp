// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/supervisor/options.h>

#include <charconv>
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

namespace ks::supervisor {

namespace {

/// Sets `field` from the value of `option`, refusing an empty value and a
/// repeated option.
std::expected<void, std::string> set_path(std::optional<std::filesystem::path>& field,
                                          std::string_view option, std::string_view value) {
    if (field) {
        return std::unexpected{std::format("kool_spin: {} given twice", option)};
    }
    if (value.empty()) {
        return std::unexpected{std::format("kool_spin: {} needs a path", option)};
    }
    field = std::filesystem::path{value};
    return {};
}

/// Sets the budget from --track-memory's value: a whole number of MiB from
/// kMinTrackMemoryMib to kMaxTrackMemoryMib (specs/track-loading/spec.md
/// requirement 9).
std::expected<void, std::string> set_track_memory(std::optional<std::uint64_t>& field,
                                                  std::string_view value) {
    if (field) {
        return std::unexpected{std::string{"kool_spin: --track-memory given twice"}};
    }
    std::uint64_t mib = 0;
    const char* const first = std::to_address(value.begin());
    const char* const last = std::to_address(value.end());
    const auto [end, error] = std::from_chars(first, last, mib);
    if (value.empty() || error != std::errc{} || end != last || mib < kMinTrackMemoryMib ||
        mib > kMaxTrackMemoryMib) {
        return std::unexpected{
            std::format("kool_spin: --track-memory takes a whole number of MiB from {} to {}, "
                        "not '{}'",
                        kMinTrackMemoryMib, kMaxTrackMemoryMib, value)};
    }
    field = mib;
    return {};
}

/// Sets `field` from a whole number of milliseconds, 1 to 3,600,000, for
/// --crash-window and --ready-timeout (specs/engine-restart/spec.md
/// requirement "test-only options").
std::expected<void, std::string> set_milliseconds(std::optional<std::uint64_t>& field,
                                                  std::string_view option, std::string_view value) {
    if (field) {
        return std::unexpected{std::format("kool_spin: {} given twice", option)};
    }
    std::uint64_t ms = 0;
    const char* const first = std::to_address(value.begin());
    const char* const last = std::to_address(value.end());
    const auto [end, error] = std::from_chars(first, last, ms);
    if (value.empty() || error != std::errc{} || end != last || ms < 1 || ms > 3'600'000) {
        return std::unexpected{std::format(
            "kool_spin: {} takes a whole number of milliseconds from 1 to 3600000, not '{}'",
            option, value)};
    }
    field = ms;
    return {};
}

/// The field a path option sets, or null if `option` isn't one.
std::optional<std::filesystem::path>* path_field(Options& options, std::string_view option) {
    if (option == "--runtime-dir") {
        return &options.runtime_dir;
    }
    if (option == "--log-dir") {
        return &options.log_dir;
    }
    if (option == "--engine") {
        return &options.engine;
    }
    if (option == "--worker") {
        return &options.worker;
    }
    return nullptr;
}

/// True if `arg` is an option that takes a value.
bool takes_value(Options& options, std::string_view arg) {
    return path_field(options, arg) != nullptr || arg == "--track-memory" ||
           arg == "--crash-window" || arg == "--ready-timeout";
}

/// Applies `value` to the option `arg`, one of those takes_value() accepts.
std::expected<void, std::string> apply_value(Options& options, std::string_view arg,
                                             std::string_view value) {
    if (std::optional<std::filesystem::path>* const field = path_field(options, arg)) {
        return set_path(*field, arg, value);
    }
    if (arg == "--crash-window") {
        return set_milliseconds(options.crash_window_ms, arg, value);
    }
    if (arg == "--ready-timeout") {
        return set_milliseconds(options.ready_timeout_ms, arg, value);
    }
    return set_track_memory(options.track_memory_mib, value);
}

} // namespace

std::string usage() {
    return "Usage: kool_spin [options] [-- engine options]\n"
           "\n"
           "Starts the engine, ks-engine, and lets clients connect to it and load\n"
           "tracks, which ks-worker decodes. Stops everything with a fade-out on\n"
           "SIGINT (Ctrl-C), SIGTERM, or SIGHUP, or when a client asks.\n"
           "\n"
           "Options:\n"
           "  --runtime-dir <path>  the runtime directory (default: $XDG_RUNTIME_DIR/kool_spin\n"
           "                        on Linux, $TMPDIR/kool_spin on macOS)\n"
           "  --log-dir <path>      the folder for each session's log files (default:\n"
           "                        $XDG_STATE_HOME/kool_spin/logs or\n"
           "                        ~/.local/state/kool_spin/logs on Linux,\n"
           "                        ~/Library/Logs/kool_spin on macOS)\n"
           "  --engine <path>       the ks-engine program (default: ks-engine next to\n"
           "                        kool_spin)\n"
           "  --worker <path>       the ks-worker program (default: ks-worker next to\n"
           "                        kool_spin)\n"
           "  --track-memory <MiB>  the memory decoded tracks may use, 1 to 1048576\n"
           "                        (default: half of physical memory)\n"
           "  --recording           keep a recording segment of what a paced engine plays,\n"
           "                        for tests (clients get it with OpenRecording)\n"
           "  --crash-window <ms>   for tests: the window in which engine exits count\n"
           "                        together (default: 30000)\n"
           "  --ready-timeout <ms>  for tests: how long a new engine has to send Ready\n"
           "                        (default: 10000)\n"
           "  --help                print this text and exit\n"
           "\n"
           "Everything after -- goes to ks-engine: --backend, --device, --rate,\n"
           "--channels, and --period (see ks-engine --help).\n"
           "\n"
           "Exit codes: 0 stopped cleanly, 1 couldn't start, 2 usage error, 3 the\n"
           "engine kept failing or didn't stop.\n";
}

std::expected<Options, std::string> parse_options(std::span<const std::string_view> args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            options.help = true;
            return options;
        }
        if (arg == "--") {
            for (const std::string_view engine_arg : args.subspan(i + 1)) {
                options.engine_args.emplace_back(engine_arg);
            }
            return options;
        }
        if (arg == "--recording") {
            if (options.recording) {
                return std::unexpected{std::string{"kool_spin: --recording given twice"}};
            }
            options.recording = true;
            continue;
        }
        if (!takes_value(options, arg)) {
            return std::unexpected{std::format("kool_spin: unexpected argument '{}'", arg)};
        }
        if (i + 1 >= args.size()) {
            return std::unexpected{std::format("kool_spin: {} needs a value", arg)};
        }
        const std::expected<void, std::string> set = apply_value(options, arg, args[++i]);
        if (!set) {
            return std::unexpected{set.error()};
        }
    }
    return options;
}

} // namespace ks::supervisor
