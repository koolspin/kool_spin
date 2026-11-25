// SPDX-License-Identifier: GPL-3.0-or-later
// ks_lock_holder: a helper for the supervisor-crash acceptance tests.
//
//   ks_lock_holder <mode> [<runtime-dir>]
//
// With a runtime directory, it takes the engine lock there, as ks-engine
// would; without one, it's a bystander. Then it waits: in mode `stops` until
// SIGTERM, which ends it with status 0, and in mode `ignores` whatever SIGTERM
// says. It ends after 60 s anyway, so that nothing a failed test leaves runs
// for long.

#include <ks/ipc/runtime_dir.h>

#include <unistd.h>

#include <csignal>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace {

extern "C" void end(int /*signal*/) {
    ::_exit(0);
}

int hold(std::span<char*> args) {
    if (args.size() < 2) {
        return 2;
    }
    // Before it takes the lock, so that a test that sees it held finds this
    // in place.
    const bool ignores = std::string_view{args[1]} == "ignores";
    static_cast<void>(std::signal(SIGTERM, ignores ? SIG_IGN : &end));

    std::optional<ks::ipc::LockFile> lock;
    if (args.size() > 2) {
        std::expected<ks::ipc::LockFile, ks::ipc::LockFile::Failure> acquired =
            ks::ipc::LockFile::acquire(args[2], ks::ipc::kEngineLockName);
        if (!acquired) {
            return 1;
        }
        lock = std::move(*acquired);
    }
    ::alarm(60);
    for (;;) {
        ::pause();
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        return hold(std::span<char*>{argv, static_cast<std::size_t>(argc)});
    } catch (...) {
        return 2;
    }
}
