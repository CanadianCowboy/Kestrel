#include "storage/json.h"

#include <string>

namespace kestrel::storage::json {
namespace {

// A parser over a fixed span. Holds no ownership and allocates only for the
// strings it finds, so a document that is rejected early costs almost nothing.
class Parser {
public:
    Parser(std::string_view text, std::string& error) : m_text(text), m_error(error) {}

    std::optional<Value> run() {
        skipWhitespace();
        std::optional<Value> value = parseValue(0);
        if (!value.has_value()) {
            return std::nullopt;
        }
        skipWhitespace();
        if (m_at != m_text.size()) {
            fail("there is more after the document");
            return std::nullopt;
        }
        return value;
    }

private:
    [[nodiscard]] bool atEnd() const { return m_at >= m_text.size(); }
    [[nodiscard]] char peek() const { return m_text[m_at]; }

    void skipWhitespace() {
        while (!atEnd()) {
            const char character = peek();
            if (character != ' ' && character != '\t' && character != '\n' && character != '\r') {
                break;
            }
            ++m_at;
        }
    }

    void fail(const std::string& what) {
        if (!m_error.empty()) {
            return; // Keep the first failure: it is the one that explains the rest.
        }
        m_error = what + " at byte " + std::to_string(m_at);
    }

    std::optional<Value> parseValue(unsigned depth) {
        if (atEnd()) {
            fail("the document ends where a value was expected");
            return std::nullopt;
        }
        switch (peek()) {
        case '{':
            return parseObject(depth);
        case '[':
            return parseArray(depth);
        case '"': {
            std::optional<std::string> text = parseString();
            if (!text.has_value()) {
                return std::nullopt;
            }
            return Value::makeString(std::move(*text));
        }
        case 't':
            return parseLiteral("true", Value::makeBoolean(true));
        case 'f':
            return parseLiteral("false", Value::makeBoolean(false));
        case 'n':
            return parseLiteral("null", Value::makeNull());
        default:
            return parseInteger();
        }
    }

    std::optional<Value> parseLiteral(std::string_view word, Value result) {
        if (m_text.compare(m_at, word.size(), word) != 0) {
            fail("expected " + std::string(word));
            return std::nullopt;
        }
        m_at += word.size();
        return result;
    }

    std::optional<Value> parseObject(unsigned depth) {
        if (depth >= Value::kMaxDepth) {
            fail("the document nests more than " + std::to_string(Value::kMaxDepth) + " deep");
            return std::nullopt;
        }
        ++m_at; // '{'
        Value object = Value::makeObject();
        skipWhitespace();
        if (!atEnd() && peek() == '}') {
            ++m_at;
            return object;
        }
        for (;;) {
            skipWhitespace();
            if (atEnd() || peek() != '"') {
                fail("expected a member name in double quotes");
                return std::nullopt;
            }
            std::optional<std::string> key = parseString();
            if (!key.has_value()) {
                return std::nullopt;
            }
            // A name that appears twice is rejected rather than resolved. This
            // reader is for documents a person can open and edit, and
            // {"a":1,"a":2} is a file whose text says one thing and whose
            // meaning is another -- whichever way the tie is broken, it is not
            // the way a reader of the file would guess. Silently keeping one of
            // them is also how a document stops round-tripping: the duplicate
            // collapses, and the bytes the checksum was taken over are not the
            // bytes that come back out.
            if (object.has(*key)) {
                fail("the member name \"" + *key + "\" appears more than once");
                return std::nullopt;
            }
            skipWhitespace();
            if (atEnd() || peek() != ':') {
                fail("expected a colon after the member name");
                return std::nullopt;
            }
            ++m_at;
            skipWhitespace();
            std::optional<Value> value = parseValue(depth + 1);
            if (!value.has_value()) {
                return std::nullopt;
            }
            object.set(std::move(*key), std::move(*value));
            skipWhitespace();
            if (atEnd()) {
                fail("the object is not closed");
                return std::nullopt;
            }
            if (peek() == ',') {
                ++m_at;
                continue;
            }
            if (peek() == '}') {
                ++m_at;
                return object;
            }
            fail("expected a comma or a closing brace");
            return std::nullopt;
        }
    }

    std::optional<Value> parseArray(unsigned depth) {
        if (depth >= Value::kMaxDepth) {
            fail("the document nests more than " + std::to_string(Value::kMaxDepth) + " deep");
            return std::nullopt;
        }
        ++m_at; // '['
        Value array = Value::makeArray();
        skipWhitespace();
        if (!atEnd() && peek() == ']') {
            ++m_at;
            return array;
        }
        for (;;) {
            skipWhitespace();
            std::optional<Value> element = parseValue(depth + 1);
            if (!element.has_value()) {
                return std::nullopt;
            }
            array.push(std::move(*element));
            skipWhitespace();
            if (atEnd()) {
                fail("the array is not closed");
                return std::nullopt;
            }
            if (peek() == ',') {
                ++m_at;
                continue;
            }
            if (peek() == ']') {
                ++m_at;
                return array;
            }
            fail("expected a comma or a closing bracket");
            return std::nullopt;
        }
    }

    // Appends one code point as UTF-8. Everything above the ASCII range reaches
    // the output as raw bytes, which is what keeps a conversation containing an
    // emoji or an accent a readable length instead of six bytes per character.
    static void appendUtf8(std::string& out, std::uint32_t code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    std::optional<std::uint32_t> parseHex4() {
        if (m_at + 4 > m_text.size()) {
            fail("a \\u escape needs four hex digits");
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char character = m_text[m_at + static_cast<std::size_t>(i)];
            value <<= 4;
            if (character >= '0' && character <= '9') {
                value |= static_cast<std::uint32_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                value |= static_cast<std::uint32_t>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                value |= static_cast<std::uint32_t>(character - 'A' + 10);
            } else {
                fail("a \\u escape needs four hex digits");
                return std::nullopt;
            }
        }
        m_at += 4;
        return value;
    }

    std::optional<std::string> parseString() {
        ++m_at; // opening quote
        std::string out;
        for (;;) {
            if (atEnd()) {
                fail("the string is not closed");
                return std::nullopt;
            }
            const unsigned char character = static_cast<unsigned char>(peek());
            if (character == '"') {
                ++m_at;
                return out;
            }
            if (character < 0x20) {
                // A raw newline inside a string is the single most common way a
                // hand-edited file stops being parseable, and JSON does not
                // allow it. Say so rather than accepting it and producing a
                // document that will not round-trip.
                fail("a control character has to be escaped inside a string");
                return std::nullopt;
            }
            if (character != '\\') {
                out.push_back(static_cast<char>(character));
                ++m_at;
                continue;
            }
            ++m_at; // backslash
            if (atEnd()) {
                fail("the string ends in the middle of an escape");
                return std::nullopt;
            }
            const char escape = peek();
            ++m_at;
            switch (escape) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                std::optional<std::uint32_t> code = parseHex4();
                if (!code.has_value()) {
                    return std::nullopt;
                }
                std::uint32_t point = *code;
                if (point >= 0xD800 && point <= 0xDBFF) {
                    // A high surrogate is only half a character. Taking it on
                    // its own would produce a lone surrogate, which is not
                    // valid UTF-8 and would not survive a round trip.
                    if (m_at + 1 >= m_text.size() || m_text[m_at] != '\\' ||
                        m_text[m_at + 1] != 'u') {
                        fail("a high surrogate has to be followed by a low one");
                        return std::nullopt;
                    }
                    m_at += 2;
                    std::optional<std::uint32_t> low = parseHex4();
                    if (!low.has_value()) {
                        return std::nullopt;
                    }
                    if (*low < 0xDC00 || *low > 0xDFFF) {
                        fail("a high surrogate has to be followed by a low one");
                        return std::nullopt;
                    }
                    point = 0x10000 + ((point - 0xD800) << 10) + (*low - 0xDC00);
                } else if (point >= 0xDC00 && point <= 0xDFFF) {
                    fail("a low surrogate with no high surrogate before it");
                    return std::nullopt;
                }
                appendUtf8(out, point);
                break;
            }
            default:
                fail("unknown escape sequence");
                return std::nullopt;
            }
        }
    }

    std::optional<Value> parseInteger() {
        const std::size_t start = m_at;
        if (!atEnd() && peek() == '-') {
            ++m_at;
        }
        const std::size_t digitsStart = m_at;
        while (!atEnd() && peek() >= '0' && peek() <= '9') {
            ++m_at;
        }
        if (m_at == digitsStart) {
            fail("expected a value");
            return std::nullopt;
        }
        // Leading zeros, a fraction or an exponent are all rejected, because
        // each of them has more than one spelling and this document's integrity
        // check is taken over its exact bytes.
        if (m_text[digitsStart] == '0' && m_at - digitsStart > 1) {
            fail("a number must not have a leading zero");
            return std::nullopt;
        }
        if (!atEnd()) {
            const char character = peek();
            if (character == '.' || character == 'e' || character == 'E') {
                fail("only whole numbers are supported");
                return std::nullopt;
            }
        }
        const std::string text(m_text.substr(start, m_at - start));
        const bool negative = text.front() == '-';
        // Accumulated as an unsigned magnitude, because the magnitude of
        // INT64_MIN is one larger than INT64_MAX and building it in a signed
        // type would reject the one value the serialiser is able to write.
        std::uint64_t magnitude = 0;
        for (const char character : text) {
            const int digit = character == '-' ? 0 : character - '0';
            const std::uint64_t limit =
                negative ? static_cast<std::uint64_t>(INT64_MAX) + 1u
                         : static_cast<std::uint64_t>(INT64_MAX);
            if (magnitude > (limit - static_cast<std::uint64_t>(digit)) / 10u) {
                fail("the number is too large");
                return std::nullopt;
            }
            magnitude = magnitude * 10u + static_cast<std::uint64_t>(digit);
        }
        if (negative) {
            return Value::makeInteger(magnitude == static_cast<std::uint64_t>(INT64_MAX) + 1u
                                          ? INT64_MIN
                                          : -static_cast<std::int64_t>(magnitude));
        }
        return Value::makeInteger(static_cast<std::int64_t>(magnitude));
    }

    std::string_view m_text;
    std::string& m_error;
    std::size_t m_at = 0;
};

void writeEscapedString(const std::string& text, std::string& out) {
    out.push_back('"');
    for (const char character : text) {
        const unsigned char byte = static_cast<unsigned char>(character);
        switch (byte) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (byte < 0x20) {
                static constexpr char kDigits[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(kDigits[(byte >> 4) & 0x0F]);
                out.push_back(kDigits[byte & 0x0F]);
            } else {
                out.push_back(character);
            }
            break;
        }
    }
    out.push_back('"');
}

void writeValue(const Value& value, std::string& out, unsigned indent, unsigned depth) {
    const bool pretty = indent > 0;
    const auto newlineAndIndent = [&](unsigned at) {
        if (pretty) {
            out.push_back('\n');
            out.append(static_cast<std::size_t>(indent * at), ' ');
        }
    };
    switch (value.type()) {
    case Value::Type::Null:
        out += "null";
        break;
    case Value::Type::Boolean:
        out += value.asBoolean().value_or(false) ? "true" : "false";
        break;
    case Value::Type::Integer:
        out += std::to_string(value.asInteger().value_or(0));
        break;
    case Value::Type::String:
        writeEscapedString(std::string(value.asString().value_or(std::string_view())), out);
        break;
    case Value::Type::Array: {
        const std::vector<Value>* elements = value.asArray();
        if (elements == nullptr) {
            out += "[]";
            break;
        }
        if (elements->empty()) {
            out += "[]";
            break;
        }
        out.push_back('[');
        bool first = true;
        for (const Value& element : *elements) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            newlineAndIndent(depth + 1);
            writeValue(element, out, indent, depth + 1);
        }
        newlineAndIndent(depth);
        out.push_back(']');
        break;
    }
    case Value::Type::Object: {
        const std::vector<Member>* members = value.asObject();
        if (members == nullptr || members->empty()) {
            out += "{}";
            break;
        }
        out.push_back('{');
        bool first = true;
        for (const Member& member : *members) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            newlineAndIndent(depth + 1);
            writeEscapedString(member.key, out);
            out.push_back(':');
            if (pretty) {
                out.push_back(' ');
            }
            writeValue(member.value, out, indent, depth + 1);
        }
        newlineAndIndent(depth);
        out.push_back('}');
        break;
    }
    }
}

} // namespace

std::optional<bool> Value::asBoolean() const {
    if (m_type != Type::Boolean) {
        return std::nullopt;
    }
    return m_boolean;
}

std::optional<std::int64_t> Value::asInteger() const {
    if (m_type != Type::Integer) {
        return std::nullopt;
    }
    return m_integer;
}

std::optional<std::string_view> Value::asString() const {
    if (m_type != Type::String) {
        return std::nullopt;
    }
    return std::string_view(m_string);
}

const std::vector<Value>* Value::asArray() const {
    return m_type == Type::Array ? &m_array : nullptr;
}

const std::vector<Member>* Value::asObject() const {
    return m_type == Type::Object ? &m_object : nullptr;
}

const Value* Value::find(std::string_view key) const {
    if (m_type != Type::Object) {
        return nullptr;
    }
    for (const Member& member : m_object) {
        if (member.key == key) {
            return &member.value;
        }
    }
    return nullptr;
}

Value Value::makeNull() {
    return Value();
}

Value Value::makeBoolean(bool value) {
    Value result;
    result.m_type = Type::Boolean;
    result.m_boolean = value;
    return result;
}

Value Value::makeInteger(std::int64_t value) {
    Value result;
    result.m_type = Type::Integer;
    result.m_integer = value;
    return result;
}

Value Value::makeString(std::string value) {
    Value result;
    result.m_type = Type::String;
    result.m_string = std::move(value);
    return result;
}

Value Value::makeArray() {
    Value result;
    result.m_type = Type::Array;
    return result;
}

Value Value::makeObject() {
    Value result;
    result.m_type = Type::Object;
    return result;
}

void Value::push(Value value) {
    if (m_type != Type::Array) {
        return;
    }
    m_array.push_back(std::move(value));
}

void Value::set(std::string key, Value value) {
    if (m_type != Type::Object) {
        return;
    }
    for (Member& member : m_object) {
        if (member.key == key) {
            member.value = std::move(value);
            return;
        }
    }
    m_object.push_back(Member{std::move(key), std::move(value)});
}

bool Value::erase(std::string_view key) {
    if (m_type != Type::Object) {
        return false;
    }
    for (auto it = m_object.begin(); it != m_object.end(); ++it) {
        if (it->key == key) {
            // erase-then-rotate rather than a loop of swaps: a member vector
            // holds a Value each, and moving them around by index is a way to
            // lose one.
            m_object.erase(it);
            return true;
        }
    }
    return false;
}

std::string Value::serialize() const {
    std::string out;
    writeValue(*this, out, 0, 0);
    return out;
}

std::string Value::serializeIndented(unsigned indent) const {
    std::string out;
    writeValue(*this, out, indent, 0);
    return out;
}

std::optional<Value> Value::parse(std::string_view text, std::string& error) {
    error.clear();
    Parser parser(text, error);
    std::optional<Value> value = parser.run();
    if (!value.has_value() && error.empty()) {
        // Belt and braces: a parser that fails without saying why is the one
        // failure mode this whole error string exists to prevent.
        error = "the document could not be read";
    }
    return value;
}

} // namespace kestrel::storage::json
