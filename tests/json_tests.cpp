#include "storage/json.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

using kestrel::storage::json::Value;

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        return;
    }
    std::cout << "  FAIL " << what << "\n";
    ++g_failures;
}

std::string textOf(const Value& value) {
    const auto text = value.asString();
    return text.has_value() ? std::string(*text) : std::string("<not a string>");
}

// Parses, and reports where it went wrong rather than letting a nullopt
// propagate into the assertion that was meant to be under test.
Value parseOrFail(const std::string& document) {
    std::string error;
    auto parsed = Value::parse(document, error);
    if (!parsed.has_value()) {
        check(false, "expected this to parse: " + document + " (" + error + ")");
        return Value::makeNull();
    }
    return std::move(*parsed);
}

void testScalarsRoundTrip() {
    std::string error;
    const Value document = parseOrFail(
        R"({"nothing":null,"yes":true,"no":false,"count":42,"negative":-7,"who":"kestrel"})");

    check(document.isObject(), "an object is an object");
    check(document.find("nothing") != nullptr && document.find("nothing")->isNull(),
          "null survives");
    check(document.find("yes")->asBoolean().value_or(false), "true survives");
    check(document.find("no")->asBoolean().value_or(true) == false, "false survives");
    check(document.find("count")->asInteger().value_or(0) == 42, "an integer survives");
    check(document.find("negative")->asInteger().value_or(0) == -7, "a negative survives");
    check(textOf(*document.find("who")) == "kestrel", "a string survives");

    // A type mismatch has to be answerable without throwing, because the input
    // is a file and a file can hold anything.
    check(!document.find("count")->asString().has_value(), "an integer is not a string");
    check(!document.find("who")->asInteger().has_value(), "a string is not an integer");
    check(document.find("missing") == nullptr, "an absent key is absent, not empty");
    check(!document.has("missing"), "and has() agrees");

    // Round trip: the same document must come back to the same bytes.
    check(document.serialize() == parseOrFail(document.serialize()).serialize(),
          "a document survives a round trip byte for byte");
    (void)error;
}

void testSerializationIsCanonical() {
    Value object = Value::makeObject();
    object.set("zebra", Value::makeInteger(1));
    object.set("apple", Value::makeInteger(2));
    object.set("mango", Value::makeInteger(3));

    // Insertion order, not sorted: a profile's integrity check is taken over
    // these bytes, so the order has to be a property of the value rather than
    // of a hash table.
    check(object.serialize() == R"({"zebra":1,"apple":2,"mango":3})",
          "members keep the order they were written in, got " + object.serialize());

    // Setting an existing key replaces it where it stands rather than adding a
    // second copy, so building the same document twice gives the same bytes.
    object.set("zebra", Value::makeInteger(9));
    check(object.serialize() == R"({"zebra":9,"apple":2,"mango":3})",
          "setting an existing key replaces it in place, got " + object.serialize());
    check(object.asObject() != nullptr && object.asObject()->size() == 3,
          "and does not grow the object");

    // The indented form is for a person to read; it has to mean the same thing.
    const std::string pretty = object.serializeIndented(2);
    check(pretty.find('\n') != std::string::npos, "the indented form has line breaks");
    check(parseOrFail(pretty).serialize() == object.serialize(),
          "the indented form parses back to the same document");
}

void testEscapes() {
    Value object = Value::makeObject();
    object.set("quote", Value::makeString("he said \"hi\""));
    object.set("backslash", Value::makeString("C:\\models\\qwen.gguf"));
    object.set("lines", Value::makeString("one\ntwo\r\tthree"));
    object.set("control", Value::makeString(std::string("bell\x07 end", 9)));
    object.set("accent", Value::makeString("caf\xc3\xa9"));
    object.set("emoji", Value::makeString("\xf0\x9f\xa6\x9d"));

    const std::string serialized = object.serialize();
    const Value back = parseOrFail(serialized);

    check(textOf(*back.find("quote")) == "he said \"hi\"", "a quote survives");
    check(textOf(*back.find("backslash")) == "C:\\models\\qwen.gguf", "a backslash survives");
    check(textOf(*back.find("lines")) == "one\ntwo\r\tthree", "control escapes survive");
    check(textOf(*back.find("control")) == std::string("bell\x07 end", 9),
          "an unusual control character survives");
    check(textOf(*back.find("accent")) == "caf\xc3\xa9", "an accented letter survives");
    check(textOf(*back.find("emoji")) == "\xf0\x9f\xa6\x9d", "an emoji survives");
    check(back.serialize() == serialized, "and the whole object round-trips");

    // A surrogate pair is how JSON spells anything outside the basic plane.
    // Taking it on its own would be a lone surrogate, which is not text.
    std::string error;
    const auto rocket = Value::parse(R"("\ud83d\ude80")", error);
    check(rocket.has_value(), "a surrogate pair parses");
    if (rocket.has_value()) {
        check(textOf(*rocket) == "\xf0\x9f\x9a\x80", "and becomes one code point");
    }
}

void testTheSmallestAndLargestIntegers() {
    // The serialiser can write INT64_MIN, so the reader has to be able to read
    // it back. Building the magnitude in a signed type is the obvious way to
    // get this wrong, and the obvious way looks correct until this exact value
    // arrives.
    const std::string document =
        R"({"smallest":-9223372036854775808,"largest":9223372036854775807})";
    const Value parsed = parseOrFail(document);
    check(parsed.find("smallest") != nullptr &&
              parsed.find("smallest")->asInteger().value_or(0) == INT64_MIN,
          "the smallest 64-bit integer round-trips");
    check(parsed.find("largest") != nullptr &&
              parsed.find("largest")->asInteger().value_or(0) == INT64_MAX,
          "and the largest");
    check(parsed.serialize() == document, "and the document comes back byte for byte");

    std::string error;
    check(!Value::parse(R"({"too_big":9223372036854775808})", error).has_value(),
          "one past the largest is rejected");
    check(!Value::parse(R"({"too_small":-9223372036854775809})", error).has_value(),
          "and one past the smallest");
}

void testMalformedDocumentsAreRejected() {
    struct Case {
        const char* document;
        const char* because;
    };
    // Every one of these is something a hand-edited or truncated file looks
    // like. Accepting any of them would mean a corrupt profile loads as a
    // partial one, and the user cannot tell the difference.
    const std::vector<Case> cases = {
        {"", "nothing at all"},
        {"{", "an object that is never closed"},
        {"[1, 2", "an array that is never closed"},
        {R"("unterminated)", "a string that is never closed"},
        {"{\"a\" 1}", "a missing colon"},
        {"{a: 1}", "an unquoted member name"},
        {"[1, 2] trailing", "content after the document"},
        {R"({"a":1,"a":2})", "a member name that appears twice"},
        {R"({"a":1,"b":2,"a":3})", "a repeated name further in"},
        {R"("bad \q escape")", "an unknown escape"},
        {"\"a\nb\"", "a raw newline inside a string"},
        {"{\"a\":01}", "a leading zero"},
        {"{\"a\":1.5}", "a fraction"},
        {"{\"a\":1e5}", "an exponent"},
        {"{\"a\":+1}", "a leading plus"},
        {R"("\ud83d")", "a high surrogate with nothing after it"},
        {R"("\ude80")", "a low surrogate with nothing before it"},
        {"tru", "a truncated literal"},
        {"[1,]", "a trailing comma"},
    };
    for (const Case& one : cases) {
        std::string error;
        const auto parsed = Value::parse(one.document, error);
        check(!parsed.has_value(), std::string("rejected: ") + one.because);
        check(!error.empty(), std::string("and says why: ") + one.because);
    }
}

void testNestingIsBounded() {
    // A file can contain a great many open brackets. Without a ceiling the
    // parser would recurse until the stack ran out, and a profile is exactly
    // the kind of file that arrives from outside.
    const std::string tooDeep(200, '[');
    std::string error;
    check(!Value::parse(tooDeep, error).has_value(), "a deeply nested document is rejected");
    check(error.find("nests") != std::string::npos, "and the reason is the nesting");

    const std::string shallow(Value::kMaxDepth - 1, '[');
    check(Value::parse(shallow + "0" + std::string(Value::kMaxDepth - 1, ']'), error).has_value(),
          "and a document inside the limit is fine");
}

void testErrorNamesWhereItWentWrong() {
    std::string error;
    check(!Value::parse("{\"a\":1, \"b\":}", error).has_value(), "a missing value is rejected");
    // "invalid JSON" has never once helped anyone find the problem. An offset
    // is the difference between a five-minute fix and an afternoon.
    check(error.find("byte") != std::string::npos,
          "the error names a byte offset, got: " + error);
    check(error.find("byte 12") != std::string::npos, "and the offset is the one that failed: " + error);
}

void testEmptyContainers() {
    const Value object = parseOrFail("{}");
    const Value array = parseOrFail("[]");
    check(object.isObject() && object.asObject()->empty(), "an empty object is empty");
    check(array.isArray() && array.asArray()->empty(), "an empty array is empty");
    check(object.serialize() == "{}", "and stays compact");
    check(array.serialize() == "[]", "and stays compact");
    // Whitespace around a document is not content.
    check(parseOrFail("  \n\t {\"a\" : 1} \n ").serialize() == "{\"a\":1}",
          "surrounding whitespace is ignored");
}

void testDeeplyNestedRealDocument() {
    // The shape a conversation actually takes, since that is what this exists
    // for: a list of conversations, each with a list of messages.
    Value message = Value::makeObject();
    message.set("role", Value::makeString("user"));
    message.set("content", Value::makeString("what is the weather?"));
    Value conversation = Value::makeObject();
    conversation.set("id", Value::makeInteger(1));
    Value messages = Value::makeArray();
    messages.push(std::move(message));
    conversation.set("messages", std::move(messages));
    Value conversations = Value::makeArray();
    conversations.push(std::move(conversation));
    Value document = Value::makeObject();
    document.set("version", Value::makeInteger(1));
    document.set("conversations", std::move(conversations));

    const std::string serialized = document.serialize();
    const Value back = parseOrFail(serialized);
    const Value* list = back.find("conversations");
    check(list != nullptr && list->isArray() && list->asArray()->size() == 1,
          "the conversation list survives");
    if (list != nullptr && list->asArray() != nullptr && !list->asArray()->empty()) {
        const Value& first = list->asArray()->front();
        const Value* messagesBack = first.find("messages");
        check(messagesBack != nullptr && messagesBack->asArray() != nullptr &&
                  messagesBack->asArray()->size() == 1,
              "and so does the message list inside it");
        if (messagesBack != nullptr && messagesBack->asArray() != nullptr &&
            !messagesBack->asArray()->empty()) {
            const Value* role = messagesBack->asArray()->front().find("role");
            check(role != nullptr && textOf(*role) == "user", "and the message itself");
        }
    }
    check(back.serialize() == serialized, "and the whole document round-trips");
}

} // namespace

int main() {
    std::cout << "json tests\n";
    testScalarsRoundTrip();
    testSerializationIsCanonical();
    testEscapes();
    testTheSmallestAndLargestIntegers();
    testMalformedDocumentsAreRejected();
    testNestingIsBounded();
    testErrorNamesWhereItWentWrong();
    testEmptyContainers();
    testDeeplyNestedRealDocument();

    if (g_failures == 0) {
        std::cout << "json tests passed\n";
        return 0;
    }
    std::cout << g_failures << " json check(s) failed\n";
    return 1;
}
