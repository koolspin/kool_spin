// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/supervisor/options.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
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

} // namespace

std::string usage() {
    return "Usage: kool_spin [options] [-- engine options]\n"
           "\n"
           "Starts the engine, ks-engine, and lets clients connect to it. Stops\n"
           "everything with a fade-out on SIGINT (Ctrl-C), SIGTERM, or SIGHUP, or\n"
           "when a client asks.\n"
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
           "  --help                print this text and exit\n"
           "\n"
           "Everything after -- goes to ks-engine: --backend, --device, --rate,\n"
           "--channels, and --period (see ks-engine --help).\n"
           "\n"
           "Exit codes: 0 stopped cleanly, 1 couldn't start, 2 usage error, 3 the\n"
           "engine ended unexpectedly or didn't stop.\n";
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
        if (arg == "--runtime-dir" || arg == "--log-dir" || arg == "--engine") {
            if (i + 1 >= args.size()) {
                return std::unexpected{std::format("kool_spin: {} needs a value", arg)};
            }
            std::optional<std::filesystem::path>* field = &options.runtime_dir;
            if (arg == "--engine") {
                field = &options.engine;
            } else if (arg == "--log-dir") {
                field = &options.log_dir;
            }
            if (auto set = set_path(*field, arg, args[++i]); !set) {
                return std::unexpected{std::move(set.error())};
            }
            continue;
        }
        return std::unexpected{std::format("kool_spin: unexpected argument '{}'", arg)};
    }
    return options;
}

} // namespace ks::supervisor
