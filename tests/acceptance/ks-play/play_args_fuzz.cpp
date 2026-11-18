// SPDX-License-Identifier: GPL-3.0-or-later
// ks-play's argument parser's fuzz target (specs/ks-play/spec.md requirement
// 33). Built only under the fuzz preset.
//
// The input is split into arguments at NUL bytes. The parser must never
// crash, and every command line it accepts must be one that requirement 22's
// table allows.

#include <ks/ipc/command.h>
#include <ks/ipc/state.h>
#include <ks/play_app/options.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

namespace app = ks::play_app;
using ks::ipc::CommandType;

/// True if `command`'s real is one its type allows: never NaN or +inf, and
/// -inf only for the three controls that accept it.
bool real_allowed(const ks::client::Command& command) {
    const double value = std::bit_cast<double>(command.payload[0]);
    if (std::isnan(value) || value == std::numeric_limits<double>::infinity()) {
        return false;
    }
    if (value == -std::numeric_limits<double>::infinity()) {
        return command.type == CommandType::SetTrim || command.type == CommandType::SetMainLevel ||
               command.type == CommandType::SetHeadphoneLevel;
    }
    return true;
}

bool control_allowed(const ks::client::Command& command) {
    switch (command.type) {
    case CommandType::SetPlaying:
    case CommandType::SetChannelCue:
        return command.target < ks::ipc::kStateDecks && command.payload[0] <= 1;
    case CommandType::SetRate:
    case CommandType::Seek:
    case CommandType::SetCuePoint:
    case CommandType::SetTrim:
    case CommandType::SetFader:
        return command.target < ks::ipc::kStateDecks && real_allowed(command);
    case CommandType::SetCrossfaderAssign:
        return command.target < ks::ipc::kStateDecks && command.payload[0] <= 2;
    case CommandType::SetCrossfader:
    case CommandType::SetCrossfaderCurve:
    case CommandType::SetMainLevel:
    case CommandType::SetHeadphoneLevel:
    case CommandType::SetCueMix:
        return command.target == 0 && real_allowed(command);
    case CommandType::SetBusMap:
        return command.target == 0;
    }
    return false;
}

bool accepted_is_allowed(const app::Invocation& invocation) {
    if (invocation.name.empty() || invocation.name.size() > 64) {
        return false;
    }
    if (const auto* const load = std::get_if<app::Load>(&invocation.request)) {
        return load->deck < ks::ipc::kStateDecks && !load->path.empty() &&
               load->path.front() == '/';
    }
    if (const auto* const unload = std::get_if<app::Unload>(&invocation.request)) {
        return unload->deck < ks::ipc::kStateDecks;
    }
    if (const auto* const control = std::get_if<app::Control>(&invocation.request)) {
        return control_allowed(control->command);
    }
    if (const auto* const status = std::get_if<app::Status>(&invocation.request)) {
        return status->interval_ms >= 50 && status->interval_ms <= 60'000;
    }
    return true;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::vector<std::string> storage;
    std::string current;
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] == 0) {
            storage.push_back(current);
            current.clear();
        } else {
            current.push_back(static_cast<char>(data[i]));
        }
    }
    storage.push_back(current);
    std::vector<std::string_view> args;
    args.reserve(storage.size());
    for (const std::string& arg : storage) {
        args.emplace_back(arg);
    }
    const auto parsed = app::parse(args, "/work", "/run");
    if (parsed && !accepted_is_allowed(*parsed)) {
        std::abort();
    }
    return 0;
}
