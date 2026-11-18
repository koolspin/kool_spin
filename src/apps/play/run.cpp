// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/play_app/run.h>

#include <ostream>

// A stub until the build loop writes it.

namespace ks::play_app {

int run(const Invocation& /*invocation*/, std::ostream& /*out*/, std::ostream& err) {
    err << "ks-play: not written yet\n";
    return kExitFailed;
}

int main_entry(int /*argc*/, const char* const* /*argv*/, std::ostream& /*out*/,
               std::ostream& err) {
    err << "ks-play: not written yet\n";
    return kExitFailed;
}

} // namespace ks::play_app
