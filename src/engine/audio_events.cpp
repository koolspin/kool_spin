// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/audio_events.h>

namespace ks::engine {

// Stubs until the maintainer approves the acceptance tests. They use no state
// yet, so clang-tidy would make them static.

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): a stub
std::vector<EventLine> EventLines::add(const AudioEvent& /*event*/, Clock::time_point /*now*/) {
    return {};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): a stub
std::vector<EventLine> EventLines::dropped(std::uint64_t /*total*/, Clock::time_point /*now*/) {
    return {};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): a stub
std::vector<EventLine> EventLines::tick(Clock::time_point /*now*/) {
    return {};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): a stub
std::vector<EventLine> EventLines::flush(Clock::time_point /*now*/) {
    return {};
}

} // namespace ks::engine
