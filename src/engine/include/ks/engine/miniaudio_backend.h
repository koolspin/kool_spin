// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Playing through a device with miniaudio.

#include <ks/engine/audio_backend.h>
#include <ks/engine/render_core.h>

#include <cstdint>
#include <expected>
#include <memory>

namespace ks::engine {

/// Which of miniaudio's backends to try.
enum class MiniaudioBackends : std::uint8_t {
    /// The system's, in miniaudio's default order.
    System,
    /// miniaudio's null backend: no device, but it calls back in real time.
    /// For tests.
    Null,
};

struct MiniaudioConfig {
    MiniaudioBackends backends = MiniaudioBackends::System;
    /// 0: the device's own channel count.
    std::uint32_t channels = 0;
    /// 0: the device's own sample rate.
    std::uint32_t sample_rate = 0;
    /// 0: the backend's choice.
    std::uint32_t period_frames = 0;
};

/// Opens the default playback device, rendering through `core`, which must
/// outlive the backend. The backend isn't started. Everything its callback
/// uses is allocated here.
std::expected<std::unique_ptr<AudioBackend>, BackendError>
open_miniaudio(RenderCore& core, const MiniaudioConfig& config);

} // namespace ks::engine
