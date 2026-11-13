// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/harness/encode.h>

#include <ks/engine/command.h>
#include <ks/ipc/command.h>

#include <cstdint>

namespace ks::harness {

// Stub (specs/harness/spec.md requirement 17).
ipc::CommandSlot encode(const engine::Command& /*command*/, std::uint64_t /*sequence*/) noexcept {
    return {};
}

} // namespace ks::harness
