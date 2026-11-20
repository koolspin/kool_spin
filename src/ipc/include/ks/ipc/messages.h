// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// The socket protocol's message types (specs/socket-protocol/spec.md
/// requirements 20 to 24).
///
/// Every type has a number that's never reused or renumbered (0 is never a
/// type), a body with fixed fields in a fixed order, an exact descriptor
/// count, and a rule for its request ID: non-zero for a request, the
/// request's ID for a reply, or 0 for a notification.
///
/// Bodies: integers are little-endian at their stated width, with no padding.
/// Text is a 32-bit byte length, then that many bytes of UTF-8 with no NUL,
/// overlong encodings, surrogates, or anything above U+10FFFF; each text field
/// has a maximum length. No field is optional, and a body must be used
/// exactly.
///
/// | Type    | Number | Body                          | Descriptors | Request ID          |
/// |---------|--------|-------------------------------|-------------|---------------------|
/// | Ping    | 1      | Empty                         | 0           | Non-zero            |
/// | Pong    | 2      | Empty                         | 0           | The Ping's          |
/// | Error   | 3      | Code (16 bits); text (≤ 1024) | 0           | The request's, or 0 |
/// | Hello   | 4      | Name: text (1 to 64)          | 0           | Non-zero            |
/// | Welcome | 5      | Empty                         | 1           | The Hello's         |
/// | Stop    | 6      | Empty                         | 0           | Non-zero            |
/// | Done    | 7      | Empty                         | 0           | The Stop's          |
/// | Ready   | 8      | Empty                         | 0           | 0                   |
/// | OpenRing   | 9   | Empty                         | 0           | Non-zero            |
/// | RingOpened | 10  | Slot (32 bits)                | 1           | The OpenRing's      |
/// | Attach     | 11  | Empty                         | 1           | Non-zero            |
/// | Attached   | 12  | Slot (32 bits)                | 0           | The Attach's        |
/// | Detach     | 13  | Slot (32 bits)                | 0           | Non-zero            |
/// | Detached   | 14  | Empty                         | 0           | The Detach's        |
/// | Probe      | 15  | Empty                         | 1           | Non-zero            |
/// | Probed     | 16  | Rate, channels (32 bits each); estimated frames (64) | 0 | The Probe's |
/// | Decode     | 17  | Max frames (64 bits)          | 1           | Non-zero            |
/// | Decoded    | 18  | Frames (64 bits); status (8)  | 0           | The Decode's        |
/// | LoadFile   | 19  | Deck (32 bits); path: text (1 to 4096) | 0  | Non-zero            |
/// | FileLoaded | 20  | Track ID (64); rate (32); estimated frames (64) | 0 | The LoadFile's |
/// | LoadTrack  | 21  | Track ID (64); deck (32); rate (32) | 1     | Non-zero            |
/// | TrackLoaded | 22 | Empty                         | 0           | The LoadTrack's     |
/// | TrackReleased | 23 | Track ID (64 bits)          | 0           | 0                   |
/// | UnloadDeck | 24    | Deck (32 bits)                | 0           | Non-zero            |
/// | DeckUnloaded | 25  | Empty                         | 0           | The UnloadDeck's    |
/// | DecodeEnded | 26   | Track ID (64); frames (64); status (8) | 0  | 0                   |
///
/// Types 4 to 8 come from specs/processes/spec.md requirement 6, types 9 to
/// 14 from specs/client-rings/spec.md requirement 3, types 15 to 18 from
/// specs/decode-worker/spec.md requirement 8, and types 19 to 23 from
/// specs/track-loading/spec.md requirement 1, and types 24 to 26 from
/// specs/ks-play/spec.md requirement 1. A slot is a client slot's index,
/// below kStateClients; a deck is a deck's index, below kStateDecks. Later
/// steps add types with the same rules, each in its own spec.

#include <ks/ipc/frame.h>
#include <ks/ipc/state.h>
#include <ks/ipc/track_segment.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <variant>

namespace ks::ipc {

/// The message types. The values are part of the protocol: never renumber or
/// reuse them. A frame carries the type in 16 bits.
enum class MessageType : std::uint8_t {
    Ping = 1,           ///< Asks the peer to answer with a Pong.
    Pong = 2,           ///< Answers a Ping.
    Error = 3,          ///< Reports that something the peer sent was refused.
    Hello = 4,          ///< A client introduces itself to the supervisor.
    Welcome = 5,        ///< The supervisor answers a Hello with the engine-state segment.
    Stop = 6,           ///< Asks the receiver to fade out and stop.
    Done = 7,           ///< Answers a Stop, once the receiver has stopped.
    Ready = 8,          ///< The engine is playing. A notification, with request ID 0.
    OpenRing = 9,       ///< A client asks the supervisor for a command ring.
    RingOpened = 10,    ///< The supervisor answers an OpenRing with the ring segment.
    Attach = 11,        ///< The supervisor asks the engine to attach a ring segment.
    Attached = 12,      ///< The engine answers an Attach with the ring's client slot.
    Detach = 13,        ///< The supervisor asks the engine to detach a client slot's ring.
    Detached = 14,      ///< The engine answers a Detach once the slot is free again.
    Probe = 15,         ///< The supervisor asks a worker about a file.
    Probed = 16,        ///< A worker answers a Probe with the file's rate, channels, and length.
    Decode = 17,        ///< The supervisor asks a worker to decode the file into a track segment.
    Decoded = 18,       ///< A worker answers a Decode once decoding has ended.
    LoadFile = 19,      ///< A client asks the supervisor to load a file onto a deck.
    FileLoaded = 20,    ///< The supervisor answers a LoadFile once the engine has the track.
    LoadTrack = 21,     ///< The supervisor asks the engine to put a track segment on a deck.
    TrackLoaded = 22,   ///< The engine answers a LoadTrack once it has given the deck the track.
    TrackReleased = 23, ///< The engine tells the supervisor that no deck holds a track any more.
    UnloadDeck =
        24, ///< A client asks the supervisor, and the supervisor the engine, to unload a deck.
    DeckUnloaded = 25,    ///< Answers an UnloadDeck.
    DecodeEnded = 26,     ///< The supervisor tells a client how decoding of its load ended.
    OpenRecording = 27,   ///< A client asks the supervisor for the recording segment.
    RecordingOpened = 28, ///< The supervisor answers an OpenRecording with the segment.
};

/// What an Error message reports. The values are part of the protocol. The
/// message carries the code in 16 bits.
enum class ErrorCode : std::uint8_t {
    /// The sender refused something the receiver sent, and is about to close
    /// the connection.
    BadMessage = 1,
    /// The sender doesn't accept that message from this peer, and carries on.
    NotSupported = 2,
    /// The sender takes that request from this peer, but can't do it now, and
    /// carries on. The peer may ask again later (specs/client-rings/spec.md
    /// requirement 4).
    Unavailable = 3,
    /// The sender won't do what was asked, for the reason in the text
    /// (specs/decode-worker/spec.md requirement 8).
    Refused = 4,
};

/// The most channels a Probed message reports.
inline constexpr std::uint32_t kMaxProbedChannels = 64;

/// The longest path a LoadFile message carries, in bytes. A path has at least
/// one byte, and starts with '/'.
inline constexpr std::size_t kMaxPathSize = 4096;

/// The longest text an Error message carries, in bytes.
inline constexpr std::size_t kMaxErrorTextSize = 1024;

/// The longest name a Hello message carries, in bytes. A name has at least
/// one byte.
inline constexpr std::size_t kMaxClientNameSize = 64;

/// The bodies of the message types.
namespace msg {

struct Ping {
    friend bool operator==(const Ping&, const Ping&) = default;
};

struct Pong {
    friend bool operator==(const Pong&, const Pong&) = default;
};

struct Error {
    ErrorCode code = ErrorCode::BadMessage;
    std::string text; ///< UTF-8, up to kMaxErrorTextSize bytes.

    friend bool operator==(const Error&, const Error&) = default;
};

/// A client's first message to the supervisor.
struct Hello {
    std::string name; ///< UTF-8, 1 to kMaxClientNameSize bytes, for reports.

    friend bool operator==(const Hello&, const Hello&) = default;
};

/// The supervisor's answer to a Hello. Its one descriptor is the
/// engine-state segment, which the client maps read-only.
struct Welcome {
    friend bool operator==(const Welcome&, const Welcome&) = default;
};

/// Asks the receiver to fade out and stop: a client asks the supervisor, and
/// the supervisor asks the engine.
struct Stop {
    friend bool operator==(const Stop&, const Stop&) = default;
};

/// Answers a Stop, once the receiver has stopped.
struct Done {
    friend bool operator==(const Done&, const Done&) = default;
};

/// The engine tells the supervisor that it's playing.
struct Ready {
    friend bool operator==(const Ready&, const Ready&) = default;
};

/// A client asks the supervisor for a command ring.
struct OpenRing {
    friend bool operator==(const OpenRing&, const OpenRing&) = default;
};

/// The supervisor's answer to an OpenRing. Its one descriptor is the
/// command-ring segment, which the client maps read-write.
struct RingOpened {
    std::uint32_t slot = 0; ///< The ring's client slot, below kStateClients.

    friend bool operator==(const RingOpened&, const RingOpened&) = default;
};

/// The supervisor asks the engine to attach a ring, in a given client slot
/// (specs/engine-restart/spec.md requirement 6). Its one descriptor is the
/// command-ring segment.
struct Attach {
    std::uint32_t slot = 0; ///< The slot the ring must take, below kStateClients.

    friend bool operator==(const Attach&, const Attach&) = default;
};

/// The engine's answer to an Attach.
struct Attached {
    std::uint32_t slot = 0; ///< The ring's client slot, below kStateClients.

    friend bool operator==(const Attached&, const Attached&) = default;
};

/// The supervisor asks the engine to detach a client slot's ring.
struct Detach {
    std::uint32_t slot = 0; ///< Below kStateClients.

    friend bool operator==(const Detach&, const Detach&) = default;
};

/// The engine's answer to a Detach, once the slot is free again.
struct Detached {
    friend bool operator==(const Detached&, const Detached&) = default;
};

/// The supervisor asks a worker about a file. Its one descriptor is the file,
/// open for reading.
struct Probe {
    friend bool operator==(const Probe&, const Probe&) = default;
};

/// A worker's answer to a Probe.
struct Probed {
    /// The file's rate in Hz, from kMinTrackRate to kMaxTrackRate.
    std::uint32_t sample_rate = 0;
    /// The file's channels, from 1 to kMaxProbedChannels.
    std::uint32_t channels = 0;
    /// The file's length in frames as the decoder estimates it before
    /// decoding, at most kMaxTrackFrames; 0 if it can't.
    std::uint64_t estimated_frames = 0;

    friend bool operator==(const Probed&, const Probed&) = default;
};

/// The supervisor asks a worker to decode the file it probed. Its one
/// descriptor is the track segment.
struct Decode {
    /// The most frames to decode, from 1 to kMaxTrackFrames.
    std::uint64_t max_frames = 0;

    friend bool operator==(const Decode&, const Decode&) = default;
};

/// A worker's answer to a Decode, once decoding has ended.
struct Decoded {
    /// The frames decoded, at most kMaxTrackFrames.
    std::uint64_t frames = 0;
    /// How decoding ended: any TrackStatus but Decoding.
    TrackStatus status = TrackStatus::Complete;

    friend bool operator==(const Decoded&, const Decoded&) = default;
};

/// A client asks the supervisor to load a file onto a deck.
struct LoadFile {
    /// The deck's index, below kStateDecks.
    std::uint32_t deck = 0;
    /// An absolute path: UTF-8, 1 to kMaxPathSize bytes, starting with '/'.
    std::string path;

    friend bool operator==(const LoadFile&, const LoadFile&) = default;
};

/// The supervisor's answer to a LoadFile, once the engine has the track. The
/// rate and the estimate are what the worker probed.
struct FileLoaded {
    /// The track's ID, never 0: the deck's state block reports it.
    std::uint64_t track_id = 0;
    /// The file's rate in Hz, from kMinTrackRate to kMaxTrackRate.
    std::uint32_t sample_rate = 0;
    /// The file's length in frames as the decoder estimated it, at most
    /// kMaxTrackFrames; 0 if it couldn't.
    std::uint64_t estimated_frames = 0;

    friend bool operator==(const FileLoaded&, const FileLoaded&) = default;
};

/// The supervisor asks the engine to put a track on a deck. Its one
/// descriptor is the track segment, which the engine maps read-only.
struct LoadTrack {
    /// The track's ID, never 0.
    std::uint64_t track_id = 0;
    /// The deck's index, below kStateDecks.
    std::uint32_t deck = 0;
    /// The track's rate in Hz, from kMinTrackRate to kMaxTrackRate.
    std::uint32_t sample_rate = 0;

    friend bool operator==(const LoadTrack&, const LoadTrack&) = default;
};

/// The engine's answer to a LoadTrack, once it has given the deck the track.
struct TrackLoaded {
    friend bool operator==(const TrackLoaded&, const TrackLoaded&) = default;
};

/// The engine tells the supervisor that no deck holds a track any more, and
/// that it has unmapped the track's segment. A notification, with request
/// ID 0.
struct TrackReleased {
    /// The track's ID, never 0.
    std::uint64_t track_id = 0;

    friend bool operator==(const TrackReleased&, const TrackReleased&) = default;
};

/// Asks the receiver to unload a deck: a client asks the supervisor, and the
/// supervisor asks the engine.
struct UnloadDeck {
    /// The deck's index, below kStateDecks.
    std::uint32_t deck = 0;

    friend bool operator==(const UnloadDeck&, const UnloadDeck&) = default;
};

/// Answers an UnloadDeck, once the deck is unloading.
struct DeckUnloaded {
    friend bool operator==(const DeckUnloaded&, const DeckUnloaded&) = default;
};

/// The supervisor tells the client that asked for a load how decoding ended.
/// A notification, with request ID 0, sent after the load's FileLoaded.
struct DecodeEnded {
    /// The track's ID, never 0.
    std::uint64_t track_id = 0;
    /// The frames decoded, at most kMaxTrackFrames.
    std::uint64_t frames = 0;
    /// How decoding ended: any TrackStatus but Decoding.
    TrackStatus status = TrackStatus::Complete;

    friend bool operator==(const DecodeEnded&, const DecodeEnded&) = default;
};

/// A client asks the supervisor for the recording segment
/// (specs/engine-restart/spec.md requirement 17).
struct OpenRecording {
    friend bool operator==(const OpenRecording&, const OpenRecording&) = default;
};

/// The supervisor's answer to an OpenRecording. Its one descriptor is the
/// recording segment, which the client maps read-only.
struct RecordingOpened {
    friend bool operator==(const RecordingOpened&, const RecordingOpened&) = default;
};

} // namespace msg

/// A message: its request ID and its body.
struct Message {
    std::uint32_t request_id = 0;
    std::variant<msg::Ping, msg::Pong, msg::Error, msg::Hello, msg::Welcome, msg::Stop, msg::Done,
                 msg::Ready, msg::OpenRing, msg::RingOpened, msg::Attach, msg::Attached,
                 msg::Detach, msg::Detached, msg::Probe, msg::Probed, msg::Decode, msg::Decoded,
                 msg::LoadFile, msg::FileLoaded, msg::LoadTrack, msg::TrackLoaded,
                 msg::TrackReleased, msg::UnloadDeck, msg::DeckUnloaded, msg::DecodeEnded,
                 msg::OpenRecording, msg::RecordingOpened>
        body;

    friend bool operator==(const Message&, const Message&) = default;
};

/// Why a frame isn't a valid message (requirement 22).
enum class MessageError : std::uint8_t {
    UnknownType,    ///< Not a MessageType.
    ShortBody,      ///< The body ends before its last field does.
    LongBody,       ///< Bytes are left over after the last field.
    BadField,       ///< A field's value is invalid, such as text that isn't UTF-8.
    WrongFdCount,   ///< The type carries a different number of descriptors.
    WrongRequestId, ///< A request or reply with ID 0, or a notification with one.
};

/// The message in `frame`, or why it isn't one. A function of the frame's
/// type, request ID, descriptor count, and body alone.
[[nodiscard]] std::expected<Message, MessageError> decode(const Frame& frame);

/// The frame for `message`, or why it can't be sent (the same checks decode()
/// makes). The encoding is canonical: encode(decode(frame)) gives back
/// `frame`.
[[nodiscard]] std::expected<Frame, MessageError> encode(const Message& message);

} // namespace ks::ipc
