// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/messages.h>

#include <ks/ipc/frame.h>
#include <ks/ipc/state.h>
#include <ks/ipc/track_segment.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ks::ipc {

namespace {

/// What a message type's request ID must be (requirement 20).
enum class RequestRule : std::uint8_t {
    NonZero, ///< A request, or a reply carrying its request's ID.
    Any,     ///< An answer to a request, or 0.
    Zero,    ///< A notification.
};

/// A message type's fixed rules.
struct TypeRules {
    std::uint8_t descriptors;
    RequestRule request;
};

/// The rules for `type`, or nothing if it isn't a MessageType.
std::optional<TypeRules> rules_for(std::uint16_t type) noexcept {
    switch (type) {
    case static_cast<std::uint16_t>(MessageType::Ping):
    case static_cast<std::uint16_t>(MessageType::Pong):
    case static_cast<std::uint16_t>(MessageType::Hello):
    case static_cast<std::uint16_t>(MessageType::Stop):
    case static_cast<std::uint16_t>(MessageType::Done):
    case static_cast<std::uint16_t>(MessageType::OpenRing):
    case static_cast<std::uint16_t>(MessageType::Attached):
    case static_cast<std::uint16_t>(MessageType::Detach):
    case static_cast<std::uint16_t>(MessageType::Detached):
    case static_cast<std::uint16_t>(MessageType::Probed):
    case static_cast<std::uint16_t>(MessageType::Decoded):
    case static_cast<std::uint16_t>(MessageType::LoadFile):
    case static_cast<std::uint16_t>(MessageType::FileLoaded):
    case static_cast<std::uint16_t>(MessageType::TrackLoaded):
        return TypeRules{.descriptors = 0, .request = RequestRule::NonZero};
    case static_cast<std::uint16_t>(MessageType::Welcome):
    case static_cast<std::uint16_t>(MessageType::RingOpened):
    case static_cast<std::uint16_t>(MessageType::Attach):
    case static_cast<std::uint16_t>(MessageType::Probe):
    case static_cast<std::uint16_t>(MessageType::Decode):
    case static_cast<std::uint16_t>(MessageType::LoadTrack):
        return TypeRules{.descriptors = 1, .request = RequestRule::NonZero};
    case static_cast<std::uint16_t>(MessageType::Error):
        return TypeRules{.descriptors = 0, .request = RequestRule::Any};
    case static_cast<std::uint16_t>(MessageType::Ready):
    case static_cast<std::uint16_t>(MessageType::TrackReleased):
        return TypeRules{.descriptors = 0, .request = RequestRule::Zero};
    default:
        return std::nullopt;
    }
}

/// The length of the UTF-8 sequence that starts with `lead`, and the bits of
/// the code point it carries, or a length of 0 if `lead` can't start one.
struct Lead {
    std::size_t length;
    std::uint32_t bits;
};

Lead lead_of(std::uint32_t lead) noexcept {
    if ((lead & 0xE0U) == 0xC0U) {
        return {.length = 2, .bits = lead & 0x1FU};
    }
    if ((lead & 0xF0U) == 0xE0U) {
        return {.length = 3, .bits = lead & 0x0FU};
    }
    if ((lead & 0xF8U) == 0xF0U) {
        return {.length = 4, .bits = lead & 0x07U};
    }
    return {.length = 0, .bits = 0};
}

/// True if `text` is UTF-8 with no NUL, overlong encodings, surrogates, or
/// code points above U+10FFFF (requirement 21).
bool valid_text(std::span<const std::byte> text) noexcept {
    // The smallest code point that needs a sequence of each length.
    constexpr std::array<std::uint32_t, 5> kSmallest{0, 0, 0x80, 0x800, 0x1'0000};
    std::size_t i = 0;
    while (i < text.size()) {
        const auto first = std::to_integer<std::uint32_t>(text[i]);
        if (first == 0) {
            return false;
        }
        if (first < 0x80U) {
            ++i;
            continue;
        }
        const Lead lead = lead_of(first);
        if (lead.length == 0 || text.size() - i < lead.length) {
            return false;
        }
        std::uint32_t code_point = lead.bits;
        for (const std::byte next : text.subspan(i + 1, lead.length - 1)) {
            const auto bits = std::to_integer<std::uint32_t>(next);
            if ((bits & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (bits & 0x3FU);
        }
        const bool surrogate = code_point >= 0xD800U && code_point <= 0xDFFFU;
        if (code_point < std::span{kSmallest}[lead.length] || code_point > 0x10'FFFFU ||
            surrogate) {
            return false;
        }
        i += lead.length;
    }
    return true;
}

/// Reads a body's fields in order. Running out is ShortBody.
class BodyReader {
public:
    explicit BodyReader(std::span<const std::byte> body) noexcept : rest_{body} {}

    /// The next `size`-byte little-endian integer (at most 4 bytes).
    std::expected<std::uint32_t, MessageError> integer(std::size_t size) noexcept {
        if (rest_.size() < size) {
            return std::unexpected{MessageError::ShortBody};
        }
        std::uint32_t value = 0;
        unsigned shift = 0;
        for (const std::byte byte : rest_.first(size)) {
            value |= std::to_integer<std::uint32_t>(byte) << shift;
            shift += 8U;
        }
        rest_ = rest_.subspan(size);
        return value;
    }

    /// The next 8-byte little-endian integer.
    std::expected<std::uint64_t, MessageError> integer64() noexcept {
        const std::expected<std::uint32_t, MessageError> low = integer(4);
        if (!low) {
            return std::unexpected{low.error()};
        }
        const std::expected<std::uint32_t, MessageError> high = integer(4);
        if (!high) {
            return std::unexpected{high.error()};
        }
        return (std::uint64_t{*high} << 32U) | *low;
    }

    /// The next text field, of at most `max_size` bytes.
    std::expected<std::string, MessageError> text(std::size_t max_size) {
        const std::expected<std::uint32_t, MessageError> size = integer(4);
        if (!size) {
            return std::unexpected{size.error()};
        }
        if (*size > max_size) {
            return std::unexpected{MessageError::BadField};
        }
        if (rest_.size() < *size) {
            return std::unexpected{MessageError::ShortBody};
        }
        const std::span<const std::byte> bytes = rest_.first(*size);
        if (!valid_text(bytes)) {
            return std::unexpected{MessageError::BadField};
        }
        rest_ = rest_.subspan(*size);
        std::string result(bytes.size(), '\0');
        std::ranges::transform(bytes, result.begin(),
                               [](std::byte byte) { return static_cast<char>(byte); });
        return result;
    }

    /// True once every byte has been read.
    [[nodiscard]] bool done() const noexcept { return rest_.empty(); }

private:
    std::span<const std::byte> rest_;
};

/// True if `name` is a valid Hello name: 1 to kMaxClientNameSize bytes of
/// text (specs/processes/spec.md requirement 6).
bool valid_name(std::span<const std::byte> name) noexcept {
    return !name.empty() && name.size() <= kMaxClientNameSize && valid_text(name);
}

/// Appends `value` little-endian, in `size` bytes.
void append_integer(std::vector<std::byte>& body, std::uint32_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        body.push_back(static_cast<std::byte>(value & 0xFFU));
        value >>= 8U;
    }
}

/// Appends `value` to `body`, little-endian, in 8 bytes.
void append_integer64(std::vector<std::byte>& body, std::uint64_t value) {
    append_integer(body, static_cast<std::uint32_t>(value & 0xFFFF'FFFFU), 4);
    append_integer(body, static_cast<std::uint32_t>(value >> 32U), 4);
}

/// True if `code` is an ErrorCode.
bool valid_code(std::uint32_t code) noexcept {
    return code == static_cast<std::uint32_t>(ErrorCode::BadMessage) ||
           code == static_cast<std::uint32_t>(ErrorCode::NotSupported) ||
           code == static_cast<std::uint32_t>(ErrorCode::Unavailable) ||
           code == static_cast<std::uint32_t>(ErrorCode::Refused);
}

/// True if a Probed's fields are in range (specs/decode-worker/spec.md
/// requirement 8).
bool valid_probed(const msg::Probed& probed) noexcept {
    return probed.sample_rate >= kMinTrackRate && probed.sample_rate <= kMaxTrackRate &&
           probed.channels >= 1 && probed.channels <= kMaxProbedChannels &&
           probed.estimated_frames <= kMaxTrackFrames;
}

/// True if a Decode's limit is in range.
bool valid_decode(const msg::Decode& decode) noexcept {
    return decode.max_frames >= 1 && decode.max_frames <= kMaxTrackFrames;
}

/// True if a Decoded's fields are in range: its status says how decoding
/// ended.
bool valid_decoded(const msg::Decoded& decoded) noexcept {
    return decoded.frames <= kMaxTrackFrames &&
           (decoded.status == TrackStatus::Complete || decoded.status == TrackStatus::Truncated ||
            decoded.status == TrackStatus::Failed);
}

/// True if `rate` is a rate a track can have.
bool valid_rate(std::uint32_t rate) noexcept {
    return rate >= kMinTrackRate && rate <= kMaxTrackRate;
}

/// True if `deck` is a deck's index (specs/track-loading/spec.md
/// requirement 1).
bool valid_deck(std::uint32_t deck) noexcept {
    return deck < kStateDecks;
}

/// True if `path` is a LoadFile's path: 1 to kMaxPathSize bytes of text,
/// starting with '/'.
bool valid_path(std::span<const std::byte> path) noexcept {
    return !path.empty() && path.size() <= kMaxPathSize && path.front() == std::byte{'/'} &&
           valid_text(path);
}

/// True if a FileLoaded's fields are in range.
bool valid_file_loaded(const msg::FileLoaded& loaded) noexcept {
    return loaded.track_id != 0 && valid_rate(loaded.sample_rate) &&
           loaded.estimated_frames <= kMaxTrackFrames;
}

/// True if a LoadTrack's fields are in range.
bool valid_load_track(const msg::LoadTrack& load) noexcept {
    return load.track_id != 0 && valid_deck(load.deck) && valid_rate(load.sample_rate);
}

/// True if `slot` is a client slot's index (specs/client-rings/spec.md
/// requirement 3).
bool valid_slot(std::uint32_t slot) noexcept {
    return slot < kStateClients;
}

/// The body of a type that carries only a slot: RingOpened, Attached, or
/// Detach.
template <typename Body>
std::expected<Message, MessageError> slot_body(const Frame& frame) {
    BodyReader reader{frame.body};
    const std::expected<std::uint32_t, MessageError> slot = reader.integer(4);
    if (!slot) {
        return std::unexpected{slot.error()};
    }
    if (!valid_slot(*slot)) {
        return std::unexpected{MessageError::BadField};
    }
    if (!reader.done()) {
        return std::unexpected{MessageError::LongBody};
    }
    return Message{.request_id = frame.request_id, .body = Body{.slot = *slot}};
}

/// A Probed's body.
std::expected<Message, MessageError> probed_body(const Frame& frame) {
    BodyReader reader{frame.body};
    const std::expected<std::uint32_t, MessageError> rate = reader.integer(4);
    if (!rate) {
        return std::unexpected{rate.error()};
    }
    const std::expected<std::uint32_t, MessageError> channels = reader.integer(4);
    if (!channels) {
        return std::unexpected{channels.error()};
    }
    const std::expected<std::uint64_t, MessageError> estimated = reader.integer64();
    if (!estimated) {
        return std::unexpected{estimated.error()};
    }
    if (!reader.done()) {
        return std::unexpected{MessageError::LongBody};
    }
    const msg::Probed probed{
        .sample_rate = *rate, .channels = *channels, .estimated_frames = *estimated};
    if (!valid_probed(probed)) {
        return std::unexpected{MessageError::BadField};
    }
    return Message{.request_id = frame.request_id, .body = probed};
}

/// A Decode's body.
std::expected<Message, MessageError> decode_body(const Frame& frame) {
    BodyReader reader{frame.body};
    const std::expected<std::uint64_t, MessageError> max_frames = reader.integer64();
    if (!max_frames) {
        return std::unexpected{max_frames.error()};
    }
    if (!reader.done()) {
        return std::unexpected{MessageError::LongBody};
    }
    const msg::Decode decode{.max_frames = *max_frames};
    if (!valid_decode(decode)) {
        return std::unexpected{MessageError::BadField};
    }
    return Message{.request_id = frame.request_id, .body = decode};
}

/// A Decoded's body.
std::expected<Message, MessageError> decoded_body(const Frame& frame) {
    BodyReader reader{frame.body};
    const std::expected<std::uint64_t, MessageError> frames = reader.integer64();
    if (!frames) {
        return std::unexpected{frames.error()};
    }
    const std::expected<std::uint32_t, MessageError> status = reader.integer(1);
    if (!status) {
        return std::unexpected{status.error()};
    }
    if (!reader.done()) {
        return std::unexpected{MessageError::LongBody};
    }
    // Checked before the cast, so no value outside TrackStatus is made.
    if (*status > static_cast<std::uint32_t>(TrackStatus::Failed)) {
        return std::unexpected{MessageError::BadField};
    }
    const msg::Decoded decoded{.frames = *frames, .status = static_cast<TrackStatus>(*status)};
    if (!valid_decoded(decoded)) {
        return std::unexpected{MessageError::BadField};
    }
    return Message{.request_id = frame.request_id, .body = decoded};
}

/// A LoadFile's body: the deck, then the path as text.
std::expected<Message, MessageError> load_file_body(const Frame& frame) {
    BodyReader reader{frame.body};
    const std::expected<std::uint32_t, MessageError> deck = reader.integer(4);
    if (!deck) {
        return std::unexpected{deck.error()};
    }
    std::expected<std::string, MessageError> path = reader.text(kMaxPathSize);
    if (!path) {
        return std::unexpected{path.error()};
    }
    if (!reader.done()) {
        return std::unexpected{MessageError::LongBody};
    }
    if (!valid_deck(*deck) || !valid_path(std::as_bytes(std::span{*path}))) {
        return std::unexpected{MessageError::BadField};
    }
    return Message{.request_id = frame.request_id,
                   .body = msg::LoadFile{.deck = *deck, .path = std::move(*path)}};
}

/// A FileLoaded's body.
std::expected<Message, MessageError> file_loaded_body(const Frame& frame) {
    BodyReader reader{frame.body};
    const std::expected<std::uint64_t, MessageError> track_id = reader.integer64();
    if (!track_id) {
        return std::unexpected{track_id.error()};
    }
    const std::expected<std::uint32_t, MessageError> rate = reader.integer(4);
    if (!rate) {
        return std::unexpected{rate.error()};
    }
    const std::expected<std::uint64_t, MessageError> estimated = reader.integer64();
    if (!estimated) {
        return std::unexpected{estimated.error()};
    }
    if (!reader.done()) {
        return std::unexpected{MessageError::LongBody};
    }
    const msg::FileLoaded loaded{
        .track_id = *track_id, .sample_rate = *rate, .estimated_frames = *estimated};
    if (!valid_file_loaded(loaded)) {
        return std::unexpected{MessageError::BadField};
    }
    return Message{.request_id = frame.request_id, .body = loaded};
}

/// A LoadTrack's body.
std::expected<Message, MessageError> load_track_body(const Frame& frame) {
    BodyReader reader{frame.body};
    const std::expected<std::uint64_t, MessageError> track_id = reader.integer64();
    if (!track_id) {
        return std::unexpected{track_id.error()};
    }
    const std::expected<std::uint32_t, MessageError> deck = reader.integer(4);
    if (!deck) {
        return std::unexpected{deck.error()};
    }
    const std::expected<std::uint32_t, MessageError> rate = reader.integer(4);
    if (!rate) {
        return std::unexpected{rate.error()};
    }
    if (!reader.done()) {
        return std::unexpected{MessageError::LongBody};
    }
    const msg::LoadTrack load{.track_id = *track_id, .deck = *deck, .sample_rate = *rate};
    if (!valid_load_track(load)) {
        return std::unexpected{MessageError::BadField};
    }
    return Message{.request_id = frame.request_id, .body = load};
}

/// A TrackReleased's body.
std::expected<Message, MessageError> track_released_body(const Frame& frame) {
    BodyReader reader{frame.body};
    const std::expected<std::uint64_t, MessageError> track_id = reader.integer64();
    if (!track_id) {
        return std::unexpected{track_id.error()};
    }
    if (!reader.done()) {
        return std::unexpected{MessageError::LongBody};
    }
    if (*track_id == 0) {
        return std::unexpected{MessageError::BadField};
    }
    return Message{.request_id = frame.request_id,
                   .body = msg::TrackReleased{.track_id = *track_id}};
}

/// An Error message's body.
std::expected<msg::Error, MessageError> decode_error(std::span<const std::byte> body) {
    BodyReader reader{body};
    const std::expected<std::uint32_t, MessageError> code = reader.integer(2);
    if (!code) {
        return std::unexpected{code.error()};
    }
    if (!valid_code(*code)) {
        return std::unexpected{MessageError::BadField};
    }
    std::expected<std::string, MessageError> text = reader.text(kMaxErrorTextSize);
    if (!text) {
        return std::unexpected{text.error()};
    }
    if (!reader.done()) {
        return std::unexpected{MessageError::LongBody};
    }
    return msg::Error{.code = static_cast<ErrorCode>(*code), .text = std::move(*text)};
}

/// A Hello message's body.
std::expected<msg::Hello, MessageError> decode_hello(std::span<const std::byte> body) {
    BodyReader reader{body};
    std::expected<std::string, MessageError> name = reader.text(kMaxClientNameSize);
    if (!name) {
        return std::unexpected{name.error()};
    }
    if (name->empty()) {
        return std::unexpected{MessageError::BadField};
    }
    if (!reader.done()) {
        return std::unexpected{MessageError::LongBody};
    }
    return msg::Hello{.name = std::move(*name)};
}

/// The message of a type whose body is empty.
template <typename Body>
std::expected<Message, MessageError> empty_body(const Frame& frame) {
    if (!frame.body.empty()) {
        return std::unexpected{MessageError::LongBody};
    }
    return Message{.request_id = frame.request_id, .body = Body{}};
}

/// Appends a text field: its length in 32 bits, then its bytes.
void append_text(std::vector<std::byte>& body, std::span<const std::byte> text) {
    append_integer(body, static_cast<std::uint32_t>(text.size()), 4);
    body.insert(body.end(), text.begin(), text.end());
}

/// True if `request_id` follows `rule`.
bool request_id_follows(std::uint32_t request_id, RequestRule rule) noexcept {
    switch (rule) {
    case RequestRule::NonZero:
        return request_id != 0;
    case RequestRule::Zero:
        return request_id == 0;
    case RequestRule::Any:
        break;
    }
    return true;
}

/// The type of `message`'s body, or nothing if it has none (a variant left
/// valueless by an exception).
std::optional<MessageType> type_of(const Message& message) noexcept {
    const auto& body = message.body;
    if (std::holds_alternative<msg::Ping>(body)) {
        return MessageType::Ping;
    }
    if (std::holds_alternative<msg::Pong>(body)) {
        return MessageType::Pong;
    }
    if (std::holds_alternative<msg::Error>(body)) {
        return MessageType::Error;
    }
    if (std::holds_alternative<msg::Hello>(body)) {
        return MessageType::Hello;
    }
    if (std::holds_alternative<msg::Welcome>(body)) {
        return MessageType::Welcome;
    }
    if (std::holds_alternative<msg::Stop>(body)) {
        return MessageType::Stop;
    }
    if (std::holds_alternative<msg::Done>(body)) {
        return MessageType::Done;
    }
    if (std::holds_alternative<msg::Ready>(body)) {
        return MessageType::Ready;
    }
    if (std::holds_alternative<msg::OpenRing>(body)) {
        return MessageType::OpenRing;
    }
    if (std::holds_alternative<msg::RingOpened>(body)) {
        return MessageType::RingOpened;
    }
    if (std::holds_alternative<msg::Attach>(body)) {
        return MessageType::Attach;
    }
    if (std::holds_alternative<msg::Attached>(body)) {
        return MessageType::Attached;
    }
    if (std::holds_alternative<msg::Detach>(body)) {
        return MessageType::Detach;
    }
    if (std::holds_alternative<msg::Detached>(body)) {
        return MessageType::Detached;
    }
    if (std::holds_alternative<msg::Probe>(body)) {
        return MessageType::Probe;
    }
    if (std::holds_alternative<msg::Probed>(body)) {
        return MessageType::Probed;
    }
    if (std::holds_alternative<msg::Decode>(body)) {
        return MessageType::Decode;
    }
    if (std::holds_alternative<msg::Decoded>(body)) {
        return MessageType::Decoded;
    }
    if (std::holds_alternative<msg::LoadFile>(body)) {
        return MessageType::LoadFile;
    }
    if (std::holds_alternative<msg::FileLoaded>(body)) {
        return MessageType::FileLoaded;
    }
    if (std::holds_alternative<msg::LoadTrack>(body)) {
        return MessageType::LoadTrack;
    }
    if (std::holds_alternative<msg::TrackLoaded>(body)) {
        return MessageType::TrackLoaded;
    }
    if (std::holds_alternative<msg::TrackReleased>(body)) {
        return MessageType::TrackReleased;
    }
    return std::nullopt;
}

// Each type's fields, appended to a body for encode(). Each returns false,
// appending nothing, if a field is out of range. Types whose body is empty
// take the template.

template <typename Fields>
bool append_fields(std::vector<std::byte>& /*body*/, const Fields& /*fields*/) {
    return true;
}

bool append_fields(std::vector<std::byte>& body, const msg::Error& error) {
    const auto code = static_cast<std::uint32_t>(error.code);
    const std::span<const std::byte> text = std::as_bytes(std::span{error.text});
    if (!valid_code(code) || text.size() > kMaxErrorTextSize || !valid_text(text)) {
        return false;
    }
    append_integer(body, code, 2);
    append_text(body, text);
    return true;
}

bool append_fields(std::vector<std::byte>& body, const msg::Hello& hello) {
    const std::span<const std::byte> name = std::as_bytes(std::span{hello.name});
    if (!valid_name(name)) {
        return false;
    }
    append_text(body, name);
    return true;
}

/// A slot, for the types that carry only one.
bool append_slot(std::vector<std::byte>& body, std::uint32_t slot) {
    if (!valid_slot(slot)) {
        return false;
    }
    append_integer(body, slot, 4);
    return true;
}

bool append_fields(std::vector<std::byte>& body, const msg::RingOpened& opened) {
    return append_slot(body, opened.slot);
}

bool append_fields(std::vector<std::byte>& body, const msg::Attached& attached) {
    return append_slot(body, attached.slot);
}

bool append_fields(std::vector<std::byte>& body, const msg::Detach& detach) {
    return append_slot(body, detach.slot);
}

bool append_fields(std::vector<std::byte>& body, const msg::Probed& probed) {
    if (!valid_probed(probed)) {
        return false;
    }
    append_integer(body, probed.sample_rate, 4);
    append_integer(body, probed.channels, 4);
    append_integer64(body, probed.estimated_frames);
    return true;
}

bool append_fields(std::vector<std::byte>& body, const msg::Decode& decode) {
    if (!valid_decode(decode)) {
        return false;
    }
    append_integer64(body, decode.max_frames);
    return true;
}

bool append_fields(std::vector<std::byte>& body, const msg::Decoded& decoded) {
    if (!valid_decoded(decoded)) {
        return false;
    }
    append_integer64(body, decoded.frames);
    append_integer(body, static_cast<std::uint32_t>(decoded.status), 1);
    return true;
}

bool append_fields(std::vector<std::byte>& body, const msg::LoadFile& load) {
    const std::span<const std::byte> path = std::as_bytes(std::span{load.path});
    if (!valid_deck(load.deck) || !valid_path(path)) {
        return false;
    }
    append_integer(body, load.deck, 4);
    append_text(body, path);
    return true;
}

bool append_fields(std::vector<std::byte>& body, const msg::FileLoaded& loaded) {
    if (!valid_file_loaded(loaded)) {
        return false;
    }
    append_integer64(body, loaded.track_id);
    append_integer(body, loaded.sample_rate, 4);
    append_integer64(body, loaded.estimated_frames);
    return true;
}

bool append_fields(std::vector<std::byte>& body, const msg::LoadTrack& load) {
    if (!valid_load_track(load)) {
        return false;
    }
    append_integer64(body, load.track_id);
    append_integer(body, load.deck, 4);
    append_integer(body, load.sample_rate, 4);
    return true;
}

bool append_fields(std::vector<std::byte>& body, const msg::TrackReleased& released) {
    if (released.track_id == 0) {
        return false;
    }
    append_integer64(body, released.track_id);
    return true;
}

} // namespace

std::expected<Message, MessageError> decode(const Frame& frame) {
    const std::optional<TypeRules> rules = rules_for(frame.type);
    if (!rules) {
        return std::unexpected{MessageError::UnknownType};
    }
    if (frame.descriptor_count != rules->descriptors) {
        return std::unexpected{MessageError::WrongFdCount};
    }
    if (!request_id_follows(frame.request_id, rules->request)) {
        return std::unexpected{MessageError::WrongRequestId};
    }

    switch (static_cast<MessageType>(frame.type)) {
    case MessageType::Ping:
        return empty_body<msg::Ping>(frame);
    case MessageType::Pong:
        return empty_body<msg::Pong>(frame);
    case MessageType::Error: {
        std::expected<msg::Error, MessageError> error = decode_error(frame.body);
        if (!error) {
            return std::unexpected{error.error()};
        }
        return Message{.request_id = frame.request_id, .body = std::move(*error)};
    }
    case MessageType::Hello: {
        std::expected<msg::Hello, MessageError> hello = decode_hello(frame.body);
        if (!hello) {
            return std::unexpected{hello.error()};
        }
        return Message{.request_id = frame.request_id, .body = std::move(*hello)};
    }
    case MessageType::Welcome:
        return empty_body<msg::Welcome>(frame);
    case MessageType::Stop:
        return empty_body<msg::Stop>(frame);
    case MessageType::Done:
        return empty_body<msg::Done>(frame);
    case MessageType::Ready:
        return empty_body<msg::Ready>(frame);
    case MessageType::OpenRing:
        return empty_body<msg::OpenRing>(frame);
    case MessageType::RingOpened:
        return slot_body<msg::RingOpened>(frame);
    case MessageType::Attach:
        return empty_body<msg::Attach>(frame);
    case MessageType::Attached:
        return slot_body<msg::Attached>(frame);
    case MessageType::Detach:
        return slot_body<msg::Detach>(frame);
    case MessageType::Detached:
        return empty_body<msg::Detached>(frame);
    case MessageType::Probe:
        return empty_body<msg::Probe>(frame);
    case MessageType::Probed:
        return probed_body(frame);
    case MessageType::Decode:
        return decode_body(frame);
    case MessageType::Decoded:
        return decoded_body(frame);
    case MessageType::LoadFile:
        return load_file_body(frame);
    case MessageType::FileLoaded:
        return file_loaded_body(frame);
    case MessageType::LoadTrack:
        return load_track_body(frame);
    case MessageType::TrackLoaded:
        return empty_body<msg::TrackLoaded>(frame);
    case MessageType::TrackReleased:
        return track_released_body(frame);
    }
    return std::unexpected{MessageError::UnknownType};
}

std::expected<Frame, MessageError> encode(const Message& message) {
    Frame frame;
    frame.request_id = message.request_id;
    const std::optional<MessageType> type = type_of(message);
    if (!type) {
        return std::unexpected{MessageError::UnknownType};
    }
    frame.type = static_cast<std::uint16_t>(*type);
    const bool valid = std::visit(
        [&frame](const auto& fields) { return append_fields(frame.body, fields); }, message.body);
    if (!valid) {
        return std::unexpected{MessageError::BadField};
    }

    const std::optional<TypeRules> rules = rules_for(frame.type);
    if (!rules) {
        return std::unexpected{MessageError::UnknownType};
    }
    frame.descriptor_count = rules->descriptors;
    if (!request_id_follows(frame.request_id, rules->request)) {
        return std::unexpected{MessageError::WrongRequestId};
    }
    return frame;
}

} // namespace ks::ipc
