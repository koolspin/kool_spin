// SPDX-License-Identifier: GPL-3.0-or-later
// kool_spin: the supervisor. It starts the engine, welcomes clients, and stops
// everything with a fade-out (specs/processes/spec.md). Everything but
// argument handling is in the ks::supervisor library.

#include <ks/supervisor/options.h>
#include <ks/supervisor/run.h>

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

    const std::expected<ks::supervisor::Options, std::string> options =
        ks::supervisor::parse_options(args);
    if (!options) {
        std::cerr << options.error() << "\n\n" << ks::supervisor::usage();
        return ks::supervisor::kExitUsageError;
    }
    return ks::supervisor::run(*options, std::cout, std::cerr);
}
