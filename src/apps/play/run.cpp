// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/play_app/run.h>

#include <ks/client/command.h>
#include <ks/client/session.h>
#include <ks/client/snapshot.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/runtime_dir.h>
#include <ks/ipc/track_segment.h>

#include "status_format.h"

#include <cstdint>
#include <filesystem>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

namespace ks::play_app {
namespace {

using client::Error;
using client::ErrorKind;
using client::Milliseconds;
using client::Session;

/// How long each next_decode_ended() call waits before `load --wait` asks
/// again: `--wait` itself has no limit (requirement 24).
constexpr Milliseconds kDecodePoll{1'000};

/// How long `status --watch` waits for each interval's `Pong`.
constexpr Milliseconds kWatchPing{1'000};

std::string_view error_code_name(ipc::ErrorCode code) noexcept {
    switch (code) {
    case ipc::ErrorCode::BadMessage:
        return "bad message";
    case ipc::ErrorCode::NotSupported:
        return "not supported";
    case ipc::ErrorCode::Unavailable:
        return "unavailable";
    case ipc::ErrorCode::Refused:
        return "refused";
    }
    return "bad message";
}

std::string_view track_status_name(ipc::TrackStatus status) noexcept {
    switch (status) {
    case ipc::TrackStatus::Complete:
        return "complete";
    case ipc::TrackStatus::Truncated:
        return "truncated";
    case ipc::TrackStatus::Decoding:
    case ipc::TrackStatus::Failed:
        return "failed";
    }
    return "failed";
}

/// Reports `error` as one line on `err` (requirement 28), and returns the
/// exit status for a failure after connecting.
int report(std::ostream& err, const Error& error) {
    err << "ks-play: ";
    if (error.kind == ErrorKind::Refused) {
        err << error_code_name(error.code) << ": ";
    }
    err << error.text << '\n';
    return kExitFailed;
}

/// Ends a command whose result went to `out`: a write that failed (a closed
/// standard output, requirement 29) is a failure.
int finish(std::ostream& out, std::ostream& err) {
    out.flush();
    if (!out) {
        err << "ks-play: can't write to standard output\n";
        return kExitFailed;
    }
    return kExitOk;
}

int do_load(Session& session, const Load& load, std::ostream& out, std::ostream& err) {
    const client::Result<client::LoadedFile> loaded = session.load_file(load.deck, load.path);
    if (!loaded) {
        return report(err, loaded.error());
    }
    out << "loaded track " << loaded->track_id << " on deck " << load.deck << ": "
        << loaded->sample_rate << " Hz, estimated " << loaded->estimated_frames << " frames\n";
    if (const int written = finish(out, err); written != kExitOk || !load.wait) {
        return written;
    }
    for (;;) {
        const client::Result<ipc::msg::DecodeEnded> ended = session.next_decode_ended(kDecodePoll);
        if (!ended) {
            if (ended.error().kind == ErrorKind::Timeout) {
                continue;
            }
            return report(err, ended.error());
        }
        if (ended->track_id != loaded->track_id) {
            continue;
        }
        out << "decoded " << ended->frames << " frames: " << track_status_name(ended->status)
            << '\n';
        const int written = finish(out, err);
        if (written != kExitOk) {
            return written;
        }
        return ended->status == ipc::TrackStatus::Complete ? kExitOk : kExitFailed;
    }
}

int do_unload(Session& session, const Unload& unload, std::ostream& out, std::ostream& err) {
    if (const client::Result<void> done = session.unload_deck(unload.deck); !done) {
        return report(err, done.error());
    }
    out << "unloaded deck " << unload.deck << '\n';
    return finish(out, err);
}

int do_control(Session& session, const Control& control, std::ostream& out, std::ostream& err) {
    if (const client::Result<std::uint32_t> ring = session.open_ring(); !ring) {
        return report(err, ring.error());
    }
    const client::Result<std::uint64_t> sequence = session.send(control.command);
    if (!sequence) {
        return report(err, sequence.error());
    }
    const client::Result<client::Applied> applied = session.wait_applied(*sequence);
    if (!applied) {
        return report(err, applied.error());
    }
    switch (applied->outcome) {
    case client::Outcome::Applied:
        out << "ok\n";
        return finish(out, err);
    case client::Outcome::Rejected:
        err << "ks-play: the engine rejected the command: "
            << client::rejection_name(applied->rejection) << '\n';
        return kExitFailed;
    case client::Outcome::Corrupt:
        err << "ks-play: the ring's slot is corrupt\n";
        return kExitFailed;
    case client::Outcome::TimedOut:
        break;
    }
    err << "ks-play: timed out waiting for the engine to apply the command\n";
    return kExitFailed;
}

/// Prints one snapshot and flushes it.
int print_snapshot(Session& session, std::ostream& out, std::ostream& err) {
    const client::Result<client::Snapshot> snapshot = session.snapshot();
    if (!snapshot) {
        return report(err, snapshot.error());
    }
    format_status(*snapshot, out);
    return finish(out, err);
}

/// `status`, and `status --watch` (requirement 25). A watch asks the
/// supervisor for a `Pong` every interval: the state segment stays mapped
/// after kool_spin dies, so only the connection tells (plan, "Decisions").
int do_status(Session& session, const Status& status, std::ostream& out, std::ostream& err) {
    if (!status.watch) {
        return print_snapshot(session, out, err);
    }
    const Milliseconds interval{status.interval_ms};
    for (bool first = true;; first = false) {
        if (!first) {
            out << "---\n";
        }
        if (const int printed = print_snapshot(session, out, err); printed != kExitOk) {
            return printed;
        }
        std::this_thread::sleep_for(interval);
        if (const client::Result<void> alive = session.ping(kWatchPing); !alive) {
            return report(err, alive.error());
        }
    }
}

int do_stop(Session& session, std::ostream& out, std::ostream& err) {
    if (const client::Result<void> done = session.stop(); !done) {
        return report(err, done.error());
    }
    out << "stopped\n";
    return finish(out, err);
}

int do_ping(Session& session, std::ostream& out, std::ostream& err) {
    if (const client::Result<void> done = session.ping(); !done) {
        return report(err, done.error());
    }
    out << "pong\n";
    return finish(out, err);
}

} // namespace

int run(const Invocation& invocation, std::ostream& out, std::ostream& err) {
    if (std::holds_alternative<Help>(invocation.request)) {
        out << usage();
        return finish(out, err);
    }
    if (const Load* load = std::get_if<Load>(&invocation.request);
        load != nullptr && !std::filesystem::path{load->path}.is_absolute()) {
        // Only when the working directory couldn't be read (main_entry()).
        err << "ks-play: can't make the path absolute: " << load->path << '\n';
        return kExitFailed;
    }

    client::Result<Session> session = Session::connect(invocation.name, invocation.runtime_dir);
    if (!session) {
        if (session.error().kind == ErrorKind::NotReachable) {
            err << "ks-play: can't reach kool_spin in " << invocation.runtime_dir.string() << ": "
                << session.error().text << '\n';
            return kExitUnreachable;
        }
        return report(err, session.error());
    }

    return std::visit(
        [&](const auto& request) -> int {
            using T = std::decay_t<decltype(request)>;
            if constexpr (std::is_same_v<T, Load>) {
                return do_load(*session, request, out, err);
            } else if constexpr (std::is_same_v<T, Unload>) {
                return do_unload(*session, request, out, err);
            } else if constexpr (std::is_same_v<T, Control>) {
                return do_control(*session, request, out, err);
            } else if constexpr (std::is_same_v<T, Status>) {
                return do_status(*session, request, out, err);
            } else if constexpr (std::is_same_v<T, Stop>) {
                return do_stop(*session, out, err);
            } else if constexpr (std::is_same_v<T, Ping>) {
                return do_ping(*session, out, err);
            } else {
                static_assert(std::is_same_v<T, Help>);
                out << usage();
                return finish(out, err);
            }
        },
        invocation.request);
}

int main_entry(int argc, const char* const* argv, std::ostream& out, std::ostream& err) {
    std::vector<std::string_view> args;
    if (argc > 1 && argv != nullptr) {
        const std::span<const char* const> all{argv, static_cast<std::size_t>(argc)};
        for (const char* arg : all.subspan(1)) {
            args.emplace_back(arg != nullptr ? std::string_view{arg} : std::string_view{});
        }
    }

    // A working directory that can't be read leaves relative paths relative,
    // and run() refuses them; every other command works without one.
    std::error_code cwd_error;
    std::filesystem::path cwd = std::filesystem::current_path(cwd_error);
    if (cwd_error) {
        cwd.clear();
    }

    const std::expected<Invocation, std::string> invocation =
        parse(args, cwd, ipc::default_runtime_directory_path());
    if (!invocation) {
        err << "ks-play: " << invocation.error() << " (see ks-play --help)\n";
        return kExitUsage;
    }
    return run(*invocation, out, err);
}

} // namespace ks::play_app
