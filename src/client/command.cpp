// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/client/command.h>

// The API exists before the code (specs/ks-play/spec.md): these are stubs
// until the build loop writes them.

namespace ks::client {

Command set_playing(std::uint32_t /*deck*/, bool /*on*/) noexcept {
    return {};
}
Command set_rate(std::uint32_t /*deck*/, double /*rate*/) noexcept {
    return {};
}
Command seek(std::uint32_t /*deck*/, double /*frame*/) noexcept {
    return {};
}
Command set_cue_point(std::uint32_t /*deck*/, double /*frame*/) noexcept {
    return {};
}
Command set_trim(std::uint32_t /*channel*/, double /*db*/) noexcept {
    return {};
}
Command set_fader(std::uint32_t /*channel*/, double /*position*/) noexcept {
    return {};
}
Command set_crossfader_assign(std::uint32_t /*channel*/, std::uint32_t /*side*/) noexcept {
    return {};
}
Command set_channel_cue(std::uint32_t /*channel*/, bool /*on*/) noexcept {
    return {};
}
Command set_crossfader(double /*position*/) noexcept {
    return {};
}
Command set_crossfader_curve(double /*transition*/) noexcept {
    return {};
}
Command set_main_level(double /*db*/) noexcept {
    return {};
}
Command set_headphone_level(double /*db*/) noexcept {
    return {};
}
Command set_cue_mix(double /*position*/) noexcept {
    return {};
}
Command set_bus_map(std::uint32_t /*main*/, std::uint32_t /*cue*/) noexcept {
    return {};
}

} // namespace ks::client
