// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/ipc/messages.h>

#include <ks/ipc/frame.h>

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
        return TypeRules{.descriptors = 0, .request = RequestRule::NonZero};
    case static_cast<std::uint16_t>(MessageType::Welcome):
        return TypeRules{.descriptors = 1, .request = RequestRule::NonZero};
    case static_cast<std::uint16_t>(MessageType::Error):
        return TypeRules{.descriptors = 0, .request = RequestRule::Any};
    case static_cast<std::uint16_t>(MessageType::Ready):
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

/// True if `code` is an ErrorCode.
bool valid_code(std::uint32_t code) noexcept {
    return code == static_cast<std::uint32_t>(ErrorCode::BadMessage) ||
           code == static_cast<std::uint32_t>(ErrorCode::NotSupported);
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
    return std::nullopt;
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
    if (const auto* const error = std::get_if<msg::Error>(&message.body)) {
        const auto code = static_cast<std::uint32_t>(error->code);
        const std::span<const std::byte> text = std::as_bytes(std::span{error->text});
        if (!valid_code(code) || text.size() > kMaxErrorTextSize || !valid_text(text)) {
            return std::unexpected{MessageError::BadField};
        }
        append_integer(frame.body, code, 2);
        append_text(frame.body, text);
    } else if (const auto* const hello = std::get_if<msg::Hello>(&message.body)) {
        const std::span<const std::byte> name = std::as_bytes(std::span{hello->name});
        if (!valid_name(name)) {
            return std::unexpected{MessageError::BadField};
        }
        append_text(frame.body, name);
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
