// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/worker_app/options.h>

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace ks::worker_app {

// Stubs until the maintainer approves the acceptance tests.

std::string usage() {
    return "Usage: ks-worker --supervisor-fd <n> [--log-fd <n>]\n";
}

std::expected<Options, std::string> parse_options(std::span<const std::string_view> /*args*/) {
    return std::unexpected{std::string{"not implemented"}};
}

} // namespace ks::worker_app
