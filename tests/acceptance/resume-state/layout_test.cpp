// SPDX-License-Identifier: GPL-3.0-or-later
// Acceptance tests for specs/resume-state/spec.md: the CRC (requirement 1)
// and the resume slots' layout (requirements 2 to 5).

#include <ks/ipc/crc32.h>
#include <ks/ipc/seqlock.h>
#include <ks/ipc/state.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace {

using ks::ipc::ChannelState;
using ks::ipc::EngineState;
using ks::ipc::EngineStatus;
using ks::ipc::ResumeDeck;
using ks::ipc::ResumeState;
using ks::ipc::Seqlock;

/// The CRC-32 of `text`'s bytes, at most 64 of them.
constexpr std::uint32_t crc_of(std::string_view text) {
    std::array<std::byte, 64> buffer{};
    for (std::size_t i = 0; i < text.size() && i < buffer.size(); ++i) {
        std::span{buffer}[i] = static_cast<std::byte>(text[i]);
    }
    return ks::ipc::crc32(std::span{buffer}.first(text.size()));
}

template <typename T>
constexpr bool kSharedLayout = std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>;

constexpr std::size_t kWord = sizeof(std::uint64_t);

} // namespace

TEST_CASE("CRC-32: check values", "[resume][crc]") {
    // The standard check value, at compile time.
    static_assert(crc_of("123456789") == 0xCBF4'3926U);

    REQUIRE(crc_of("123456789") == 0xCBF4'3926U);
    REQUIRE(crc_of("The quick brown fox jumps over the lazy dog") == 0x414F'A339U);
    REQUIRE(ks::ipc::crc32({}) == 0);
}

TEST_CASE("Resume state: the layout", "[resume][layout]") {
    STATIC_REQUIRE(kSharedLayout<ResumeDeck>);
    STATIC_REQUIRE(kSharedLayout<ResumeState>);
    STATIC_REQUIRE(kSharedLayout<EngineState>);

    // Requirement 2: each field 8 bytes after the last, from 0.
    STATIC_REQUIRE(sizeof(ResumeDeck) == 5 * kWord);
    STATIC_REQUIRE(offsetof(ResumeDeck, track_id) == 0 * kWord);
    STATIC_REQUIRE(offsetof(ResumeDeck, flags) == 1 * kWord);
    STATIC_REQUIRE(offsetof(ResumeDeck, position) == 2 * kWord);
    STATIC_REQUIRE(offsetof(ResumeDeck, rate) == 3 * kWord);
    STATIC_REQUIRE(offsetof(ResumeDeck, cue) == 4 * kWord);
    STATIC_REQUIRE(ks::ipc::kResumePlaying == 1);

    // Requirement 3.
    constexpr std::size_t kDecks = ks::ipc::kStateDecks;
    constexpr std::size_t kDeckWords = 5 * kDecks;
    constexpr std::size_t kChannelWords = 4 * kDecks;
    STATIC_REQUIRE(sizeof(ChannelState) == 4 * kWord);
    STATIC_REQUIRE(offsetof(ResumeState, generation) == 0 * kWord);
    STATIC_REQUIRE(offsetof(ResumeState, host_time) == 1 * kWord);
    STATIC_REQUIRE(offsetof(ResumeState, decks) == 2 * kWord);
    STATIC_REQUIRE(sizeof(ResumeState::decks) == kDeckWords * kWord);
    STATIC_REQUIRE(offsetof(ResumeState, channels) == (2 + kDeckWords) * kWord);
    STATIC_REQUIRE(sizeof(ResumeState::channels) == kChannelWords * kWord);
    constexpr std::size_t kMixer = 2 + kDeckWords + kChannelWords;
    STATIC_REQUIRE(offsetof(ResumeState, crossfader) == (kMixer + 0) * kWord);
    STATIC_REQUIRE(offsetof(ResumeState, crossfader_transition) == (kMixer + 1) * kWord);
    STATIC_REQUIRE(offsetof(ResumeState, main_level_db) == (kMixer + 2) * kWord);
    STATIC_REQUIRE(offsetof(ResumeState, headphone_level_db) == (kMixer + 3) * kWord);
    STATIC_REQUIRE(offsetof(ResumeState, cue_mix) == (kMixer + 4) * kWord);
    STATIC_REQUIRE(offsetof(ResumeState, bus_main) == (kMixer + 5) * kWord);
    STATIC_REQUIRE(offsetof(ResumeState, bus_cue) == (kMixer + 6) * kWord);
    STATIC_REQUIRE(offsetof(ResumeState, checksum) == (kMixer + 7) * kWord);
    STATIC_REQUIRE(sizeof(ResumeState) == (kMixer + 8) * kWord);

    // Requirement 4: two slots, right after the status block, in layout
    // version 2.
    STATIC_REQUIRE(ks::ipc::kResumeSlots == 2);
    STATIC_REQUIRE(std::is_same_v<decltype(EngineState::resume),
                                  std::array<Seqlock<ResumeState>, ks::ipc::kResumeSlots>>);
    STATIC_REQUIRE(offsetof(EngineState, resume) ==
                   offsetof(EngineState, status) + sizeof(Seqlock<EngineStatus>));
    STATIC_REQUIRE(ks::ipc::kEngineStateLayoutVersion == 2);
    STATIC_REQUIRE(ks::ipc::kEngineStateSegmentSize ==
                   ks::ipc::kSegmentHeaderSize + sizeof(EngineState));
}
