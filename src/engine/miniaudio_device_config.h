// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The miniaudio configuration a device is opened with
/// (specs/miniaudio-devices/spec.md requirement 18). Internal to ks::engine;
/// the acceptance tests include it to check the configuration.

#include <ks/engine/miniaudio_backend.h>

#include <miniaudio.h>

#include <array>

namespace ks::engine {

/// A channel map with no positions: every element is MA_CHANNEL_NONE.
using BlankChannelMap = std::array<ma_channel, MA_MAX_CHANNELS>;

/// PulseAudio's PA_CHANNEL_MAP_ALSA, for miniaudio's `pulse.channelMap`.
/// miniaudio defines it, as MA_PA_CHANNEL_MAP_ALSA, only in its
/// implementation, which checks that the two agree
/// (third_party/miniaudio/miniaudio.c).
inline constexpr int kPulseAlsaChannelMap = 1;

/// A playback configuration for `config`'s channel count, sample rate, and
/// period, on the device `device_id` (null: the system default device), with
/// no callbacks. Engine output n reaches device output n:
/// - `blank_map` is its channel map, so when the device opens with the channel
///   count asked for, miniaudio gives the engine's side the device's own
///   layout and doesn't rearrange or mix channels;
/// - on PulseAudio it asks for ALSA's channel order, the layout of the sinks
///   PulseAudio and PipeWire make for analog sound cards, so the server
///   doesn't remix the stream onto them.
///
/// `blank_map` must be all MA_CHANNEL_NONE, and must outlive the call to
/// ma_device_init() that uses the configuration. It isn't changed.
[[nodiscard]] ma_device_config miniaudio_device_config(const MiniaudioConfig& config,
                                                       const ma_device_id* device_id,
                                                       BlankChannelMap& blank_map) noexcept;

} // namespace ks::engine
