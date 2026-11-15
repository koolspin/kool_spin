// SPDX-License-Identifier: GPL-3.0-or-later
// The socket protocol's fuzz target (specs/socket-protocol/spec.md requirement
// 25). Built only under the fuzz preset; "Messages: round trip" runs the same
// checks in every preset.
//
// The first byte of the input chooses how the rest is read: in what chunk
// sizes, and whether descriptor counts match the headers.

#include "protocol_checks.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> input = std::as_bytes(std::span{data, size});
    if (input.empty()) {
        return 0;
    }
    const auto control = std::to_integer<std::uint8_t>(input.front());
    const std::span<const std::byte> rest = input.subspan(1);
    if (!ks::ipc::test::stream_checks_hold(control, rest) ||
        !ks::ipc::test::frame_checks_hold(rest)) {
        std::abort();
    }
    return 0;
}
