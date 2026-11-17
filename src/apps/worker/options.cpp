// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/worker_app/options.h>

#include <charconv>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace ks::worker_app {

namespace {

/// Parses a descriptor number made only of the digits 0-9: no sign, no
/// spaces.
std::optional<int> parse_fd(std::string_view text) noexcept {
    if (text.empty() || text.front() < '0' || text.front() > '9') {
        return std::nullopt;
    }
    const char* const first = std::to_address(text.begin());
    const char* const last = std::to_address(text.end());
    int value = 0;
    const auto [end, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || end != last) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::string usage() {
    return "Usage: ks-worker --supervisor-fd <n> [--log-fd <n>]\n"
           "\n"
           "The decode worker. kool_spin starts one for each track it loads, and\n"
           "gives it these options: its socket, and its log segment. It decodes one\n"
           "file into a track segment, then exits. Without --log-fd, it reports to\n"
           "standard error.\n"
           "\n"
           "  --help  print this text and exit\n"
           "\n"
           "Exit codes: 0 the job ended, 1 couldn't start, 2 usage error.\n";
}

std::expected<Options, std::string> parse_options(std::span<const std::string_view> args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            options.help = true;
            return options;
        }
        if (arg != "--supervisor-fd" && arg != "--log-fd") {
            return std::unexpected{std::format("ks-worker: unexpected argument '{}'", arg)};
        }
        if (i + 1 >= args.size()) {
            return std::unexpected{std::format("ks-worker: {} needs a value", arg)};
        }
        const std::optional<int> fd = parse_fd(args[++i]);
        if (!fd) {
            return std::unexpected{std::format("ks-worker: {} must be a descriptor number", arg)};
        }
        std::optional<int>& field = arg == "--log-fd" ? options.log_fd : options.supervisor_fd;
        if (field) {
            return std::unexpected{std::format("ks-worker: {} given twice", arg)};
        }
        field = fd;
    }
    if (!options.supervisor_fd) {
        return std::unexpected{std::string{"ks-worker: --supervisor-fd is required"}};
    }
    return options;
}

} // namespace ks::worker_app
