// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/play_app/run.h>

#include <iostream>

int main(int argc, char** argv) {
    return ks::play_app::main_entry(argc, argv, std::cout, std::cerr);
}
