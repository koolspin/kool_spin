// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine_app/options.h>

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace ks::engine_app {

std::string usage() {
    return "Usage: ks-engine --supervisor-fd <n> --state-fd <n> --runtime-dir <path> [output "
           "options]\n";
}

std::expected<Options, std::string> parse_options(std::span<const std::string_view> /*args*/) {
    // Stub: implemented once the spec's tests are approved.
    return std::unexpected{std::string{"ks-engine: not implemented yet"}};
}

} // namespace ks::engine_app
