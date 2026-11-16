// SPDX-License-Identifier: GPL-3.0-or-later
// The log record fuzz target (specs/logs/spec.md, "Fuzzing"). Built only
// under the fuzz preset; "Log lines: format" runs the same check in every
// preset.

#include "record_check.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (!ks::logs::test::check_record_input(std::span<const std::uint8_t>{data, size})) {
        std::abort();
    }
    return 0;
}
