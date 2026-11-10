// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/miniaudio_backend.h>

namespace ks::engine {

// Stub: the acceptance tests compile against this API before it's implemented
// (specs/render-core/spec.md). The stubs make every test fail.

std::expected<std::unique_ptr<AudioBackend>, BackendError>
open_miniaudio(RenderCore& /*core*/, const MiniaudioConfig& /*config*/) {
    return std::unexpected{
        BackendError{.kind = BackendError::Kind::NoBackend, .message = "not implemented yet"}};
}

} // namespace ks::engine
