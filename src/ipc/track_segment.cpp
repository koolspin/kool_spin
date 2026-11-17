// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/track_segment.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace ks::ipc {

// Stubs until the maintainer approves the acceptance tests.

std::expected<TrackProgress, TrackHeaderError> read_track(const TrackHeader& /*header*/,
                                                          std::uint64_t /*capacity*/) {
    return std::unexpected{TrackHeaderError::BadStatus};
}

TrackWriter::TrackWriter(std::span<std::byte> /*segment*/) noexcept {}

std::uint64_t TrackWriter::capacity() const noexcept {
    return samples_.size() / 2;
}

void TrackWriter::start(std::uint32_t /*sample_rate*/) noexcept {
    static_cast<void>(header_);
}

std::span<float> TrackWriter::samples() const noexcept {
    return samples_;
}

void TrackWriter::publish(std::uint64_t /*frames*/) noexcept {}

void TrackWriter::finish(TrackStatus /*status*/, std::uint64_t /*frames*/) noexcept {}

} // namespace ks::ipc
