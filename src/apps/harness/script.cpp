// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/harness/script.h>

#include <ks/engine/command.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string_view>

namespace ks::harness {

// Stub (specs/harness/spec.md requirements 4 to 16).
std::expected<Script, ScriptError> read_script(std::string_view /*text*/,
                                               const std::filesystem::path& /*directory*/) {
    return std::unexpected{ScriptError{.line = 0, .message = "not implemented"}};
}

// Stub (specs/harness/spec.md requirement 10).
engine::Command command_for(const TrackTime& /*event*/, std::uint32_t /*sample_rate*/) noexcept {
    return {};
}

} // namespace ks::harness
