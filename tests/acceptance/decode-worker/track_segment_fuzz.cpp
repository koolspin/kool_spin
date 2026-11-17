// SPDX-License-Identifier: GPL-3.0-or-later
// The track segment header fuzz target (specs/decode-worker/spec.md
// requirement 7). Built only under the fuzz preset; "Track segment: reading
// the header" runs the same check in every preset.

#include "track_checks.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (!ks::decode::test::check_track_input(std::span<const std::uint8_t>{data, size})) {
        std::abort();
    }
    return 0;
}
