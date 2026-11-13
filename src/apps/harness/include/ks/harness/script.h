// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Reading a harness script (specs/harness/spec.md requirements 4 to 16).

#include <ks/engine/command.h>
#include <ks/ipc/command.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ks::harness {

/// The largest script, in bytes (requirement 4).
inline constexpr std::size_t kMaxScriptBytes = std::size_t{1} << 20U;
/// The most tracks a script names (requirement 8).
inline constexpr std::size_t kMaxTracks = 64;
/// The most events a script expands to (requirement 15).
inline constexpr std::size_t kMaxEvents = 100'000;
/// The latest time a script gives, in seconds (requirement 15).
inline constexpr double kMaxTime = 86'400.0;
/// The most repetitions in a repeat block (requirement 12).
inline constexpr std::uint64_t kMaxRepeatCount = 100'000;
/// A synthetic track's sample rate unless its line gives one (requirement 8).
inline constexpr std::uint32_t kDefaultSignalRate = 44'100;
/// The longest synthetic track, in seconds (requirement 8).
inline constexpr double kMaxSignalSeconds = 3'600.0;
/// The fastest clicks, in beats per minute (requirement 8).
inline constexpr double kMaxBpm = 1'000.0;

enum class SignalKind : std::uint8_t { Sine, Sweep, Noise, Clicks };

/// A synthetic track (requirements 8 and 23). Fields its kind doesn't use
/// are 0.
struct Signal {
    SignalKind kind = SignalKind::Sine;
    /// Sine: the frequency. Sweep: the start frequency.
    double frequency_hz = 0.0;
    /// Sweep: the end frequency.
    double to_hz = 0.0;
    /// Clicks: beats per minute.
    double bpm = 0.0;
    /// The track's length.
    double seconds = 0.0;
    std::uint32_t sample_rate = kDefaultSignalRate;

    bool operator==(const Signal&) const = default;
};

/// A track read from a file.
struct TrackFile {
    /// A relative path in the script, joined to the script's directory.
    std::filesystem::path path;

    bool operator==(const TrackFile&) const = default;
};

/// A `track` line.
struct TrackSpec {
    using Source = std::variant<TrackFile, Signal>;

    std::string name;
    Source source;
    /// The line it's on, counting from 1.
    std::size_t line = 0;

    bool operator==(const TrackSpec&) const = default;
};

/// `load`: gives the engine a reference to a track for a deck.
struct Load {
    /// 0-based.
    std::size_t deck = 0;
    /// An index into Script::tracks: the track with ID `track` + 1.
    std::size_t track = 0;

    bool operator==(const Load&) const = default;
};

/// `unload`.
struct Unload {
    /// 0-based.
    std::size_t deck = 0;

    bool operator==(const Unload&) const = default;
};

/// `seek` or `cuepoint`: a time in the deck's track. It becomes a command
/// once the track's sample rate is known (requirement 10; command_for()).
struct TrackTime {
    /// ipc::CommandType::Seek or ipc::CommandType::SetCuePoint.
    ipc::CommandType type = ipc::CommandType::Seek;
    /// 0-based.
    std::size_t deck = 0;
    /// Seconds into the track.
    double seconds = 0.0;
    /// The deck's track when the event runs: an index into Script::tracks.
    std::size_t track = 0;

    bool operator==(const TrackTime&) const = default;
};

/// What an event does. Every command other than `seek` and `cuepoint` is an
/// engine::Command, as decode() would return it: its deck is 0-based, its
/// bus map is in the engine's 0-based channels, and fields its type doesn't
/// use hold their defaults.
using Action = std::variant<Load, Unload, TrackTime, engine::Command>;

/// One timed line, or one line of one repetition of a repeat block.
struct Event {
    /// Seconds from the start.
    double time = 0.0;
    Action action;
    /// The line it came from, counting from 1: for a repeat block, the line
    /// inside it.
    std::size_t line = 0;

    bool operator==(const Event&) const = default;
};

/// A script, read and checked.
struct Script {
    /// In script order: tracks[i] has ID i + 1 (requirement 21).
    std::vector<TrackSpec> tracks;
    /// With repeat blocks expanded, in run order (requirement 13).
    std::vector<Event> events;
    /// The `end` time, in seconds.
    double end = 0.0;
};

/// Why a script was refused (requirement 16).
struct ScriptError {
    /// The line the error is on, counting from 1, or 0 for the script as a
    /// whole. See read_script().
    std::size_t line = 0;
    /// What's wrong, in words.
    std::string message;

    bool operator==(const ScriptError&) const = default;
};

/// Reads a script from its text (requirements 4 to 16). Relative paths in
/// `file` sources are joined to `directory`. It opens no files.
///
/// It stops at the first error. An error is on the line that's wrong,
/// except:
/// - an unknown track name: the `load` line that names it;
/// - a second `end`: the second one;
/// - an event at or after the end: that event's line, for the first such
///   event in script order;
/// - a `seek` or `cuepoint` on a deck with no track: its line, for the first
///   such event in run order;
/// - a repeat block with no `end repeat`: its `repeat` line;
/// - too many events: the line whose events go over kMaxEvents, which for a
///   repeat block is its `end repeat` line;
/// - line 0: a script over kMaxScriptBytes, and a script with no `end`.
[[nodiscard]] std::expected<Script, ScriptError>
read_script(std::string_view text, const std::filesystem::path& directory);

/// The engine command for `event`, for a track at `sample_rate`: a Seek or
/// SetCuePoint to `event.seconds` × `sample_rate` frames (requirement 10).
[[nodiscard]] engine::Command command_for(const TrackTime& event,
                                          std::uint32_t sample_rate) noexcept;

} // namespace ks::harness
