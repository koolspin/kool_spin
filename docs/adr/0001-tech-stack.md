# ADR 0001: Language, frameworks, and license

- **Status:** Accepted
- **Date:** 2026-11-08

## Context

kool_spin is an open-source DJ application, developed AI-first: AI agents write most of the code, and a human with deep C++ experience reviews it. Requirements:

- Linux and macOS required; Windows nice to have. Laptops only for now.
- Low-latency playback, with scrolling waveforms that stay in sync with what is audible.
- DJ controllers and generic MIDI input.
- Per-track BPM analysis (in practice, a beat grid: BPM plus beat phase).

The first milestones are deliberately narrow: a solid playback engine and track library first, with features built on top afterwards (see the [roadmap](../roadmap.md)).

## Decision

| Area | Choice | Notes |
|---|---|---|
| License | GPL-3.0-or-later | Allows Rubber Band, qm-dsp, libKeyFinder, Mixxx controller mappings, Ableton Link, and the GPLv3 ASIO SDK. |
| Language | C++23, limited to features supported by Clang ≥ 20, AppleClang, and GCC ≥ 13 | Ecosystem, reviewer expertise, and real-time safety tooling (below). |
| Build | CMake ≥ 3.28, Ninja, `CMakePresets.json` | |
| Dependencies | vcpkg manifest mode, pinned baseline, for everything except Qt | Same versions on Linux, macOS, and CI. Add a dependency only when a phase needs it. |
| UI | Qt 6.8 LTS or later, Qt Quick/QML; waveforms as `QQuickRhiItem` | GPU rendering via Metal on macOS and Vulkan/OpenGL on Linux. Installed with the Qt installer or aqtinstall: Ubuntu 24.04 ships Qt 6.4, which is too old. |
| Audio I/O | miniaudio, behind our own `AudioBackend` interface | Linux: JACK API through PipeWire by default, ALSA `hw:` for exclusive access. macOS: CoreAudio. |
| Decoding | FFmpeg (libavformat, libavcodec) | |
| Tags and cover art | TagLib 2.x | |
| Database | SQLite (WAL mode, FTS5) with SQLiteCpp | Hand-written SQL and versioned migrations; no ORM. |
| Beat grid and BPM | qm-dsp (vendored) | |
| Loudness | libebur128 | |
| Key lock (later) | Rubber Band v3; Signalsmith Stretch as an alternative | |
| MIDI | libremidi | Hot-plug support. |
| HID (later) | hidapi; libusb for bulk-transfer devices | |
| Controller scripting | QJSEngine, with Mixxx-compatible mapping format and control names | Reuses Mixxx's existing controller mappings. |
| Real-time plumbing | moodycamel ReaderWriterQueue; our own seqlock | |
| Tests | Catch2 v3; golden-file tests that render the engine offline | |
| Checks | clang-format, clang-tidy, ASan + UBSan, TSan, RealtimeSanitizer with `[[clang::nonblocking]]` | RT checks run in Linux CI with Clang ≥ 20. |
| CI | GitHub Actions: Ubuntu 24.04 (Clang), macOS arm64 (AppleClang) | |

### Real-time safety

Audio-thread entry points and everything they call are marked `KS_NONBLOCKING`, which expands to `[[clang::nonblocking]]` where the compiler supports it. Clang's function effect analysis then flags allocating or blocking calls at compile time, and RealtimeSanitizer (`-fsanitize=realtime`) catches them at run time. This turns the audio-thread rules from a code-review convention into a CI failure, which matters when most code is AI-written.

## Consequences

- The project is GPL-3.0-or-later and cannot later be relicensed as proprietary: qm-dsp and the Mixxx mappings are GPL-only.
- Qt is installed outside vcpkg, on developer machines and in CI.
- The engine library must not depend on Qt, so it can be tested headless and rendered offline.
- miniaudio has no ASIO support, so Windows will need another backend (likely RtAudio) behind the same interface.

## Alternatives considered

- **Rust** (cpal, symphonia, midir, egui/Slint): the borrow checker is a real safeguard for AI-written code, but the desktop GUI ecosystem is weaker for a data-heavy app, the best DSP libraries are C/C++ anyway, and the reviewer's expertise is C++.
- **JUCE 8:** an excellent all-in-one audio toolkit, but AGPLv3 or commercial, weaker for data-heavy UI such as the library browser, and its GPU path is OpenGL, which Apple has deprecated.
- **Electron or Tauri UI:** adds a process boundary between the engine and waveform rendering for no benefit.
- **RtAudio or PortAudio for audio I/O:** both viable. We chose miniaudio for device-change notifications and active maintenance. RtAudio is the likely ASIO backend for Windows.
- **ORMs** (sqlite_orm, ODB): sqlite_orm is AGPL, and ODB is heavy code generation. Hand-written SQL keeps query performance visible.
