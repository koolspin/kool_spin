// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/worker_app/run.h>

#include <ks/worker_app/options.h>

#include <ostream>

namespace ks::worker_app {

// A stub until the maintainer approves the acceptance tests.
int run(const Options& /*options*/, std::ostream& /*out*/, std::ostream& /*err*/) {
    return kExitStartFailed;
}

} // namespace ks::worker_app
