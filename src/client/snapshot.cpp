// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/client/snapshot.h>

// A stub until the build loop writes it.

namespace ks::client {

std::string_view rejection_name(Rejection /*rejection*/) noexcept {
    return {};
}

} // namespace ks::client
