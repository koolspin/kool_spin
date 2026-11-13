// SPDX-License-Identifier: GPL-3.0-or-later
// The resume-state fuzz target (specs/resume-state/spec.md requirement 19).
// Built only under the fuzz preset; "Resume state: validation properties"
// checks the same properties in every preset.

#include <ks/ipc/state.h>

#include "snapshot_support.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // Scratch space for the slot-reading check, allocated once.
    static const std::unique_ptr<ks::ipc::EngineState> state =
        std::make_unique<ks::ipc::EngineState>();
    if (!ks::engine::resume_test::check_input(std::span<const std::uint8_t>{data, size}, *state)) {
        std::abort();
    }
    return 0;
}
