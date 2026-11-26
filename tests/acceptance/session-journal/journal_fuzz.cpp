// SPDX-License-Identifier: GPL-3.0-or-later
// The session journal's fuzz target (specs/session-journal/spec.md requirement
// 7). Built only under the fuzz preset; "Journal format: fuzz checks" checks
// the same properties in every preset.

#include "journal_support.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (!ks::supervisor::journal_test::check_input(std::span<const std::uint8_t>{data, size})) {
        std::abort();
    }
    return 0;
}
