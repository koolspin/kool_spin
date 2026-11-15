// SPDX-License-Identifier: GPL-3.0-or-later
// ks-engine: the engine process, started by kool_spin (specs/processes/spec.md).
// Everything but argument handling is in the ks::engine_app library.

#include <ks/engine_app/options.h>
#include <ks/engine_app/run.h>

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

    const std::expected<ks::engine_app::Options, std::string> options =
        ks::engine_app::parse_options(args);
    if (!options) {
        std::cerr << options.error() << "\n\n" << ks::engine_app::usage();
        return ks::engine_app::kExitUsageError;
    }
    return ks::engine_app::run(*options, std::cout, std::cerr);
}
