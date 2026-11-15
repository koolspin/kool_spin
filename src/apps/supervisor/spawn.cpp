// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/supervisor/spawn.h>

#include <sys/types.h>

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

namespace ks::supervisor {

std::expected<pid_t, std::error_code> spawn(const std::filesystem::path& /*program*/,
                                            std::span<const std::string> /*args*/,
                                            std::span<const Inherit> /*fds*/) {
    // Stub: implemented once the spec's tests are approved.
    return std::unexpected{std::make_error_code(std::errc::function_not_supported)};
}

} // namespace ks::supervisor
