// SPDX-License-Identifier: GPL-3.0-or-later
// ks-worker: the decode worker, started by kool_spin for each track
// (specs/decode-worker/spec.md). Everything but argument handling is in the
// ks::worker_app library.

#include <ks/worker_app/options.h>
#include <ks/worker_app/run.h>

#include <cstddef>
#include <expected>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    const std::span<char*> arguments{argv, static_cast<std::size_t>(argc)};
    std::vector<std::string_view> args;
    for (const char* argument : arguments.subspan(1)) {
        args.emplace_back(argument);
    }

    const std::expected<ks::worker_app::Options, std::string> options =
        ks::worker_app::parse_options(args);
    if (!options) {
        std::cerr << options.error() << "\n\n" << ks::worker_app::usage();
        return ks::worker_app::kExitUsageError;
    }
    return ks::worker_app::run(*options, std::cout, std::cerr);
}
