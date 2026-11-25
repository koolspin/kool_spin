// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/ipc/state_dir.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <system_error>

namespace {

using ks::ipc::describe;

std::error_code code(std::errc error) {
    return std::make_error_code(error);
}

bool has(const std::string& text, const std::string& phrase) {
    return text.find(phrase) != std::string::npos;
}

} // namespace

// The rest is in tests/acceptance/state-folder/.
TEST_CASE("State folder: what went wrong with it", "[ipc][state-folder]") {
    const std::string writable =
        describe({.error = code(std::errc::permission_denied), .fuse = false, .writable = true});
    CHECK(has(writable, "chmod go-w"));
    const std::string fuse =
        describe({.error = code(std::errc::not_supported), .fuse = true, .writable = false});
    CHECK(has(fuse, "FUSE"));
    CHECK(has(fuse, "--state-dir"));
    const std::string network = describe({.error = code(std::errc::not_supported)});
    CHECK(has(network, "network"));
    CHECK(!has(network, "--state-dir"));
    CHECK(describe({.error = code(std::errc::permission_denied)}) ==
          code(std::errc::permission_denied).message());
}
