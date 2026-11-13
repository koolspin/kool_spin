// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/harness/options.h>

#include <atomic>
#include <expected>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace ks::harness {

// Stub (specs/harness/spec.md requirement 2).
std::string usage() {
    return {};
}

// Stub (specs/harness/spec.md requirement 2).
std::expected<Options, std::string> parse_options(std::span<const std::string_view> /*args*/) {
    return std::unexpected{std::string{"ks-harness: not implemented"}};
}

// Stub (specs/harness/spec.md requirement 3).
int run(const Options& /*options*/, std::ostream& /*out*/, std::ostream& /*err*/,
        const std::atomic<bool>& /*stop*/) {
    return kExitRuntimeError;
}

} // namespace ks::harness
