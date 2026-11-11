// SPDX-License-Identifier: GPL-3.0-or-later
// The decoder fuzz target (specs/commands/spec.md requirement 15). Built only
// under the fuzz preset; "Commands: decoding and encoding round-trip" checks
// the same properties in every preset.

#include <ks/engine/command.h>
#include <ks/ipc/command.h>

#include "codec_support.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> input{data, size};

    // Bytes to command: the first 64 bytes, zero-padded, as a slot.
    ks::ipc::CommandSlot slot{};
    if (!input.empty()) {
        std::memcpy(&slot, input.data(), std::min(input.size(), sizeof slot));
    }
    if (!ks::engine::test::slot_round_trips(slot)) {
        std::abort();
    }

    // Command to bytes: the same input chooses a valid command.
    ks::engine::test::ByteReader reader{input};
    const ks::engine::Command command = ks::engine::test::valid_command(reader);
    if (!ks::engine::test::command_round_trips(slot.sequence, command)) {
        std::abort();
    }
    return 0;
}
