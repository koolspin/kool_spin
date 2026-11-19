// SPDX-License-Identifier: GPL-3.0-or-later

#include "decode_queue.h"

#include <ks/ipc/messages.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>

namespace {

using ks::client::DecodeQueue;
using ks::ipc::Message;
using ks::ipc::TrackStatus;
using ks::ipc::msg::DecodeEnded;

Message notification(std::uint64_t track_id) {
    return Message{.request_id = 0,
                   .body = DecodeEnded{.track_id = track_id,
                                       .frames = 10 * track_id,
                                       .status = TrackStatus::Complete}};
}

} // namespace

TEST_CASE("DecodeQueue: hands out notifications oldest first") {
    DecodeQueue queue;
    CHECK_FALSE(queue.take().has_value());
    CHECK(queue.offer(notification(1)));
    CHECK(queue.offer(notification(2)));
    CHECK(queue.size() == 2);

    CHECK(queue.take() ==
          std::optional{DecodeEnded{.track_id = 1, .frames = 10, .status = TrackStatus::Complete}});
    CHECK(queue.take() ==
          std::optional{DecodeEnded{.track_id = 2, .frames = 20, .status = TrackStatus::Complete}});
    CHECK_FALSE(queue.take().has_value());
}

TEST_CASE("DecodeQueue: ignores everything but DecodeEnded notifications") {
    DecodeQueue queue;
    // A DecodeEnded with a request ID isn't a notification.
    CHECK_FALSE(queue.offer(Message{.request_id = 3, .body = DecodeEnded{.track_id = 1}}));
    CHECK_FALSE(queue.offer(Message{.request_id = 0, .body = ks::ipc::msg::Pong{}}));
    CHECK_FALSE(queue.offer(Message{.request_id = 7, .body = ks::ipc::msg::Pong{}}));
    CHECK(queue.size() == 0);
}

TEST_CASE("DecodeQueue: keeps at most 64, dropping the oldest") {
    DecodeQueue queue;
    for (std::uint64_t id = 1; id <= DecodeQueue::kCapacity + 3; ++id) {
        CHECK(queue.offer(notification(id)));
    }
    CHECK(queue.size() == DecodeQueue::kCapacity);
    // 1 to 3 were dropped.
    for (std::uint64_t id = 4; id <= DecodeQueue::kCapacity + 3; ++id) {
        const std::optional<std::uint64_t> next =
            queue.take().transform([](const DecodeEnded& ended) { return ended.track_id; });
        CHECK(next == std::optional{id});
    }
    CHECK_FALSE(queue.take().has_value());
}
