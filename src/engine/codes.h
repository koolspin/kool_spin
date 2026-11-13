// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The engine-state segment's encodings (specs/state-out/spec.md requirement
/// 1), shared by the state blocks the engine publishes and the resume slots
/// (specs/resume-state/spec.md requirement 3), so both encode alike. Internal
/// to ks::engine.

#include <ks/core/realtime.h>
#include <ks/engine/mixer.h>

#include <bit>
#include <chrono>
#include <cstdint>

namespace ks::engine::codes {

/// A real, as its bit pattern.
[[nodiscard]] inline std::uint64_t real_code(double value) noexcept KS_NONBLOCKING {
    return std::bit_cast<std::uint64_t>(value);
}

/// A host time, as nanoseconds since the steady clock's epoch.
[[nodiscard]] inline std::uint64_t
host_time_code(std::chrono::steady_clock::time_point time) noexcept KS_NONBLOCKING {
    const std::int64_t ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    return std::bit_cast<std::uint64_t>(ns);
}

[[nodiscard]] inline std::uint64_t flag_code(bool value) noexcept KS_NONBLOCKING {
    return value ? 1U : 0U;
}

/// A crossfader side: A is 0, B is 1, and thru is 2.
[[nodiscard]] inline std::uint64_t side_code(CrossfaderSide side) noexcept KS_NONBLOCKING {
    switch (side) {
    case CrossfaderSide::A:
        return 0;
    case CrossfaderSide::B:
        return 1;
    case CrossfaderSide::Thru:
        return 2;
    }
    return 2;
}

} // namespace ks::engine::codes
