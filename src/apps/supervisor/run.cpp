// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/supervisor/run.h>

#include <ks/supervisor/options.h>

#include <expected>
#include <filesystem>
#include <ostream>
#include <system_error>

namespace ks::supervisor {

std::expected<std::filesystem::path, std::error_code> default_engine_path() {
    // Stub: implemented once the spec's tests are approved.
    return std::unexpected{std::make_error_code(std::errc::function_not_supported)};
}

int run(const Options& /*options*/, std::ostream& /*out*/, std::ostream& err) {
    // Stub: implemented once the spec's tests are approved.
    err << "kool_spin: not implemented yet\n";
    return kExitStartFailed;
}

} // namespace ks::supervisor
