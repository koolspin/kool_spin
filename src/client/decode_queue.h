// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Private to ks::client: the DecodeEnded notifications a session has
/// received and not yet handed out (specs/ks-play/spec.md requirement 19).

#include <ks/ipc/messages.h>

#include <cstddef>
#include <deque>
#include <optional>
#include <variant>

namespace ks::client {

class DecodeQueue {
public:
    /// The most notifications kept: when full, the oldest is dropped.
    static constexpr std::size_t kCapacity = 64;

    /// Keeps `message` if it's a DecodeEnded notification (request ID 0).
    /// Returns whether it was kept; anything else is ignored.
    bool offer(const ipc::Message& message) {
        const auto* const ended = std::get_if<ipc::msg::DecodeEnded>(&message.body);
        if (message.request_id != 0 || ended == nullptr) {
            return false;
        }
        if (queue_.size() >= kCapacity) {
            queue_.pop_front();
        }
        queue_.push_back(*ended);
        return true;
    }

    /// The oldest notification kept, removed from the queue.
    std::optional<ipc::msg::DecodeEnded> take() {
        if (queue_.empty()) {
            return std::nullopt;
        }
        const ipc::msg::DecodeEnded first = queue_.front();
        queue_.pop_front();
        return first;
    }

    [[nodiscard]] std::size_t size() const noexcept { return queue_.size(); }

private:
    std::deque<ipc::msg::DecodeEnded> queue_;
};

} // namespace ks::client
