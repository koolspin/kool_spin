// SPDX-License-Identifier: GPL-3.0-or-later
// Compiles the miniaudio implementation exactly once (miniaudio is a header-only library).
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#if defined(MA_HAS_PULSEAUDIO)
// ks::engine asks PulseAudio for ALSA's channel order with its own copy of
// this value, kPulseAlsaChannelMap in src/engine/miniaudio_device_config.h
// (specs/miniaudio-devices/spec.md requirement 18).
_Static_assert(MA_PA_CHANNEL_MAP_ALSA == 1, "kPulseAlsaChannelMap must be MA_PA_CHANNEL_MAP_ALSA");
#endif
