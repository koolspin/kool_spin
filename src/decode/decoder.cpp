// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/decode/decoder.h>

#include <ks/ipc/fd.h>
#include <ks/ipc/track_segment.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <utility>

namespace ks::decode {

// Stubs until the maintainer approves the acceptance tests.

/// FFmpeg's contexts, once the implementation exists.
struct Decoder::State {
    DecoderInfo info;
};

FdSource::FdSource(ipc::Fd fd) noexcept : fd_{std::move(fd)} {}

std::expected<std::size_t, int> FdSource::read_at(std::uint64_t /*offset*/,
                                                  std::span<std::byte> /*buffer*/) {
    return std::unexpected{EINVAL};
}

std::expected<std::uint64_t, int> FdSource::size() {
    return std::unexpected{EINVAL};
}

std::expected<Decoder, Refusal> Decoder::open(std::unique_ptr<Source> /*source*/) {
    return std::unexpected{Refusal{.reason = RefusalReason::Unreadable, .text = "not implemented"}};
}

Decoder::Decoder(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}

Decoder::Decoder(Decoder&& other) noexcept = default;

Decoder& Decoder::operator=(Decoder&& other) noexcept = default;

Decoder::~Decoder() = default;

const DecoderInfo& Decoder::info() const noexcept {
    return state_->info;
}

std::expected<std::size_t, DecodeError> Decoder::read(std::span<float> /*out*/) {
    static_cast<void>(state_);
    return std::unexpected{DecodeError{.text = "not implemented"}};
}

std::uint64_t Decoder::skipped_packets() const noexcept {
    static_cast<void>(state_);
    return 0;
}

DecodedTrack decode_track(Decoder& /*decoder*/, ipc::TrackWriter& /*writer*/,
                          std::uint64_t /*max_frames*/, const std::function<bool()>& /*stopping*/) {
    return DecodedTrack{};
}

} // namespace ks::decode
