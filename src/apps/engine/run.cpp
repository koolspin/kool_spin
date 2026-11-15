// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine_app/run.h>

#include <ks/engine_app/options.h>

#include <ostream>

namespace ks::engine_app {

int run(const Options& /*options*/, std::ostream& /*out*/, std::ostream& err) {
    // Stub: implemented once the spec's tests are approved.
    err << "ks-engine: not implemented yet\n";
    return kExitStartFailed;
}

} // namespace ks::engine_app
