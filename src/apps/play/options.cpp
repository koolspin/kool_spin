// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/play_app/options.h>

// The API exists before the code (specs/ks-play/spec.md): stubs until the
// build loop writes them.

namespace ks::play_app {

std::expected<Invocation, std::string> parse(std::span<const std::string_view> /*args*/,
                                             const std::filesystem::path& /*cwd*/,
                                             const std::filesystem::path& /*default_runtime_dir*/) {
    return std::unexpected{std::string{"not written yet"}};
}

std::string usage() {
    return {};
}

} // namespace ks::play_app
