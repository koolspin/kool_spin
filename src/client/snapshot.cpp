// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/client/snapshot.h>

namespace ks::client {

std::string_view rejection_name(Rejection rejection) noexcept {
    switch (rejection) {
    case Rejection::None:
        return "none";
    case Rejection::UnknownType:
        return "unknown type";
    case Rejection::BadTarget:
        return "bad target";
    case Rejection::NonzeroPadding:
        return "nonzero padding";
    case Rejection::BadValue:
        return "bad value";
    }
    // Codes are validated before they become a Rejection (Session::snapshot_from()).
    return "none";
}

} // namespace ks::client
