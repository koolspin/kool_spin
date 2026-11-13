// SPDX-License-Identifier: GPL-3.0-or-later
// The script reader's fuzz target (specs/harness/spec.md, the fuzzing
// criterion). Built only under the fuzz preset; "Script: tracks and timed
// commands" checks the same properties in every preset.

#include "script_properties.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (!ks::harness::test::check_input(std::span<const std::uint8_t>{data, size})) {
        std::abort();
    }
    return 0;
}
