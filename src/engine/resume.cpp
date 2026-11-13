// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/resume.h>

#include <ks/ipc/state.h>

#include <cstddef>
#include <optional>

namespace ks::engine {

// Stubs (specs/resume-state/spec.md requirements 8 to 10): nothing is encoded,
// nothing validates, and no slot is read.

ipc::ResumeState encode(const Snapshot& /*snapshot*/) noexcept KS_NONBLOCKING {
    return ipc::ResumeState{};
}

std::optional<Snapshot> validate(const ipc::ResumeState& /*words*/,
                                 std::size_t /*slot*/) noexcept KS_NONBLOCKING {
    return std::nullopt;
}

std::optional<Snapshot> read_snapshot(ipc::EngineState& /*state*/) noexcept {
    return std::nullopt;
}

} // namespace ks::engine
