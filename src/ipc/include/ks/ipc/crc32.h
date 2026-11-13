// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// CRC-32, as zlib, PNG, and Ethernet compute it: reflected polynomial
/// 0xEDB88320, initial value 0xFFFFFFFF, and final XOR 0xFFFFFFFF
/// (specs/resume-state/spec.md requirement 1). It checks resume slots for
/// corruption (ADR 0003 §11).

#include <ks/core/realtime.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ks::ipc {

namespace detail {

/// The CRC of each byte value, one bit at a time.
inline constexpr std::array<std::uint32_t, 256> kCrc32Table = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t value = 0; value < table.size(); ++value) {
        std::uint32_t crc = value;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0 ? 0xEDB8'8320U ^ (crc >> 1U) : crc >> 1U;
        }
        std::span{table}[value] = crc;
    }
    return table;
}();

} // namespace detail

/// The CRC-32 of `bytes`. The CRC of "123456789" is 0xCBF43926, and of no
/// bytes is 0.
constexpr std::uint32_t crc32(std::span<const std::byte> bytes) noexcept KS_NONBLOCKING {
    std::uint32_t crc = 0xFFFF'FFFFU;
    for (const std::byte byte : bytes) {
        const std::uint32_t index = (crc ^ std::to_integer<std::uint32_t>(byte)) & 0xFFU;
        crc = std::span{detail::kCrc32Table}[index] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFF'FFFFU;
}

} // namespace ks::ipc
