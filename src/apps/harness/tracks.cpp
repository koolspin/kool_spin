// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/harness/tracks.h>

#include <ks/engine/track.h>
#include <ks/harness/script.h>

#include <ks_codecs.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace ks::harness {

// Stub (specs/harness/spec.md requirement 23).
std::vector<float> generate(const Signal& /*signal*/) {
    return {};
}

// Stub (specs/harness/spec.md requirement 22). The codec library is linked,
// so describing a result keeps it in the build.
std::expected<DecodedAudio, std::string> decode_file(const std::filesystem::path& /*path*/) {
    return std::unexpected{std::string{ks_codecs_describe(KS_CODECS_OPEN_FAILED)}};
}

// Stub (specs/harness/spec.md requirements 20 and 21).
std::expected<TrackSet, std::string> TrackSet::prepare(const Script& /*script*/) {
    return std::unexpected{std::string{"not implemented"}};
}

std::size_t TrackSet::size() const noexcept {
    return tracks_.size();
}

const engine::Track& TrackSet::track(std::size_t index) const {
    return *tracks_.at(index);
}

} // namespace ks::harness
