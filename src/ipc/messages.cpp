// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/messages.h>

#include <ks/ipc/frame.h>

#include <expected>

namespace ks::ipc {

// Stubs: specs/socket-protocol/spec.md isn't implemented yet.

std::expected<Message, MessageError> decode(const Frame& /*frame*/) {
    return std::unexpected{MessageError::UnknownType};
}

std::expected<Frame, MessageError> encode(const Message& /*message*/) {
    return std::unexpected{MessageError::UnknownType};
}

} // namespace ks::ipc
