// SPDX-License-Identifier: GPL-3.0-or-later
#include "miniaudio_device_config.h"

#include <ks/engine/miniaudio_backend.h>

#include <miniaudio.h>

namespace ks::engine {

ma_device_config miniaudio_device_config(const MiniaudioConfig& config,
                                         const ma_device_id* device_id,
                                         BlankChannelMap& blank_map) noexcept {
    ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
    device_config.playback.pDeviceID = device_id;
    device_config.playback.format = ma_format_f32;
    device_config.playback.channels = config.channels;       // 0: the device's own
    device_config.sampleRate = config.sample_rate;           // 0: the device's own
    device_config.periodSizeInFrames = config.period_frames; // 0: the backend's choice
    device_config.performanceProfile = ma_performance_profile_low_latency;
    // Call back with whatever the backend delivers; the engine splits it into
    // blocks itself (ADR 0003 §3).
    device_config.noFixedSizedCallback = MA_TRUE;
    // miniaudio otherwise lays out the engine's side in its own default order
    // for the channel count, and rearranges or mixes it into the device's by
    // position. A blank map takes the device's layout instead.
    device_config.playback.pChannelMap = blank_map.data();
    // PulseAudio's default order puts a 4-channel stream's third channel at
    // front centre, which the server then mixes into a 4.0 sink's front pair.
    device_config.pulse.channelMap = kPulseAlsaChannelMap;
    return device_config;
}

} // namespace ks::engine
