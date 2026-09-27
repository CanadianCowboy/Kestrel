#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kestrel::storage::json {

class Value;

// One key/value pair of an object.
//
// Declared here and defined below so that Value can hold a vector of them:
// std::vector supports an incomplete element type from C++17 onward, which is
// what lets Value contain itself.
struct Member;

// A JSON value.
//
// Deliberately not a general-purpose JSON library. It carries what a profile
// document needs -- null, bool, integer, string, array, object -- and refuses
// everything else rather than approximating it. There is no number type beyond
// a 64-bit integer because a profile has no use for one, and because a
// canonical serialisation that can print a double two different ways cannot be
// checksummed.
//
// Objects keep their members in insertion order rather than sorting them. That
// is the whole reason serialize() is a function of the value alone: a profile's
// integrity check is taken over its serialised bytes, so the same document has
// to produce the same bytes every time, on every platform, forever.
class Value {
public:
    enum class Type { Null, Boolean, Integer, String, Array, Object };

    Value() = default;

    [[nodiscard]] Type type() const { return m_type; }
    [[nodiscard]] bool isNull() const { return m_type == Type::Null; }
    [[nodiscard]] bool isBoolean() const { return m_type == Type::Boolean; }
    [[nodiscard]] bool isInteger() const { return m_type == Type::Integer; }
    [[nodiscard]] bool isString() const { return m_type == Type::String; }
    [[nodiscard]] bool isArray() const { return m_type == Type::Array; }
    [[nodiscard]] bool isObject() const { return m_type == Type::Object; }

    // Typed reads that return nullopt on a type mismatch rather than throwing
    // or asserting. A profile is a file, and a file can be anything.
    [[nodiscard]] std::optional<bool> asBoolean() const;
    [[nodiscard]] std::optional<std::int64_t> asInteger() const;
    [[nodiscard]] std::optional<std::string_view> asString() const;
    [[nodiscard]] const std::vector<Value>* asArray() const;
    [[nodiscard]] const std::vector<Member>* asObject() const;

    // The member with this key, or nullptr. A parsed document never has two of
    // the same name: parse() rejects a duplicate rather than picking one, so
    // there is no "first" or "last" to choose between here.
    [[nodiscard]] const Value* find(std::string_view key) const;
    [[nodiscard]] bool has(std::string_view key) const { return find(key) != nullptr; }

    // Builds. All static so the intended type is visible at the call site
    // rather than inferred from an assignment.
    [[nodiscard]] static Value makeNull();
    [[nodiscard]] static Value makeBoolean(bool value);
    [[nodiscard]] static Value makeInteger(std::int64_t value);
    [[nodiscard]] static Value makeString(std::string value);
    [[nodiscard]] static Value makeArray();
    [[nodiscard]] static Value makeObject();

    // Object and array construction. set() replaces an existing key where it
    // stands rather than appending a second one, so building the same logical
    // document twice produces the same bytes.
    void push(Value value);
    void set(std::string key, Value value);

    // Removes a member by key, keeping the order of the rest. Returns whether
    // there was one. Exists because a document that carries its own checksum
    // has to be able to take the checksum back off before re-serialising the
    // body, and rebuilding the object by hand to do that is a way to get the
    // order subtly wrong.
    bool erase(std::string_view key);

    // The compact canonical form: no spaces, members in insertion order, the
    // minimal escaping. This is the form the integrity check is taken over.
    [[nodiscard]] std::string serialize() const;

    // The same value, indented, for a file a person is meant to read. Parsing
    // it yields a Value that serializes() back to the compact form.
    [[nodiscard]] std::string serializeIndented(unsigned indent = 0) const;

    // Parses one complete JSON document. Trailing content other than
    // whitespace is an error, not something to skip past, and so is a member
    // name that appears twice in one object.
    //
    // On failure returns nullopt and sets `error` to a message that names the
    // byte offset, because "invalid JSON" on its own has never once helped
    // anyone find the problem.
    [[nodiscard]] static std::optional<Value> parse(std::string_view text, std::string& error);

    // The deepest an object or array may nest. A document is a file, and a file
    // can contain a million open brackets; without a ceiling the parser would
    // recurse until the stack ran out.
    static constexpr unsigned kMaxDepth = 32;

private:
    Type m_type = Type::Null;
    bool m_boolean = false;
    std::int64_t m_integer = 0;
    std::string m_string;
    std::vector<Value> m_array;
    std::vector<Member> m_object;
};

struct Member {
    std::string key;
    Value value;
};

} // namespace kestrel::storage::json
