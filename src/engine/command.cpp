// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/command.h>

namespace ks::engine {

// Stubs: the acceptance tests compile against this API before it's
// implemented (specs/commands/spec.md). decode() rejects everything, and
// apply() does nothing.

std::expected<Command, Rejection> decode(const ipc::CommandSlot& /*slot*/) noexcept KS_NONBLOCKING {
    return std::unexpected{Rejection::UnknownType};
}

void apply(const Command& /*command*/, Mixer& /*mixer*/) noexcept KS_NONBLOCKING {}

} // namespace ks::engine
