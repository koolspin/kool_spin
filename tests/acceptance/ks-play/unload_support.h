// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by specs/ks-play/spec.md's unload tests: sending an UnloadDeck and
// collecting what arrives, and reading the engine's late-block count. Built
// on the track-loading tests' helpers.

#include <ks/ipc/connection.h>
#include <ks/ipc/messages.h>
#include <ks/ipc/region.h>
#include <ks/ipc/segment.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>

#include "tracks_support.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

namespace ks::ks_play::test {

using track_loading::test::Received;

/// True if `received` holds a message of type `T` with request ID `id`.
template <typename T>
bool has(const std::vector<Received>& received, std::uint32_t id) {
    return std::ranges::any_of(received, [id](const Received& r) {
        return r.message.request_id == id && std::holds_alternative<T>(r.message.body);
    });
}

/// True if `received` holds a message of type `T`.
template <typename T>
bool has_any(const std::vector<Received>& received) {
    return std::ranges::any_of(
        received, [](const Received& r) { return std::holds_alternative<T>(r.message.body); });
}

/// The first message of type `T` in `received`, or nothing.
template <typename T>
std::optional<T> first_of(const std::vector<Received>& received) {
    for (const Received& r : received) {
        if (const auto* const body = std::get_if<T>(&r.message.body)) {
            return *body;
        }
    }
    return std::nullopt;
}

/// Sends an UnloadDeck for `deck` with request ID `id` on `connection`.
inline bool send_unload(ipc::Connection& connection, std::uint32_t id, std::uint32_t deck) {
    return processes::test::send(
        connection, ipc::Message{.request_id = id, .body = ipc::msg::UnloadDeck{.deck = deck}});
}

/// Reads `connection` for `duration`, and returns everything that arrived.
inline std::vector<Received> read_for(ipc::Connection& connection,
                                      std::chrono::milliseconds duration) {
    std::vector<Received> received;
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        std::expected<std::optional<ipc::ReceivedFrame>, ipc::ConnectionFailure> frame =
            connection.receive();
        if (!frame) {
            break;
        }
        if (*frame) {
            std::expected<ipc::Message, ipc::MessageError> message = ipc::decode((*frame)->frame);
            if (message) {
                received.push_back(
                    Received{.message = std::move(*message), .fds = std::move((*frame)->fds)});
            }
            continue;
        }
        static_cast<void>(ipc::test::wait_readable(connection.socket(), 20));
    }
    return received;
}

/// The engine's late-block count, read from the engine-state segment `fd`;
/// nothing if it can't be read.
inline std::optional<std::uint64_t> late_blocks(int fd) {
    std::expected<ipc::Region, ipc::MapFailure> region = ipc::Region::map(
        fd, ipc::SegmentKind::EngineState, ipc::kEngineStateLayoutVersion, ipc::Access::ReadOnly);
    if (!region) {
        return std::nullopt;
    }
    auto* const state = region->at<ipc::EngineState>(ipc::kSegmentHeaderSize);
    if (state == nullptr) {
        return std::nullopt;
    }
    const std::optional<ipc::EngineStatus> status =
        ipc::SeqlockReader<ipc::EngineStatus>{state->status}.try_read(1'000);
    if (!status) {
        return std::nullopt;
    }
    return status->late_blocks;
}

} // namespace ks::ks_play::test
