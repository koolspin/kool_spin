// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/supervisor/options.h>

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace ks::supervisor {

std::string usage() {
    return "Usage: kool_spin [options] [-- engine options]\n";
}

std::expected<Options, std::string> parse_options(std::span<const std::string_view> /*args*/) {
    // Stub: implemented once the spec's tests are approved.
    return std::unexpected{std::string{"kool_spin: not implemented yet"}};
}

} // namespace ks::supervisor
