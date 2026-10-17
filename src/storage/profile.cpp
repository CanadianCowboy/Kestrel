#include "storage/profile.h"

#include "storage/json.h"
#include "storage/sha256.h"

#include <limits>
#include <utility>
#include <unordered_set>

namespace kestrel::storage {
namespace {

using kestrel::storage::json::Value;

// How long a title is, in characters. Long enough to recognise a conversation
// in a list, short enough that the list is still a list.
constexpr std::size_t kTitleLength = 60;

// The document without its checksum.
//
// A free function so that building it and taking it apart are visibly two
// halves of one shape. The member order here is the order they are written in,
// and the checksum is taken over exactly this serialisation -- so changing this
// function changes every stored profile, which is why it sits behind a version.
Value bodyValue(const Profile& profile) {
    Value document = Value::makeObject();
    document.set("version", Value::makeInteger(Profile::kCurrentVersion));
    document.set("modelPath", Value::makeString(profile.modelPath));
    document.set("systemPrompt", Value::makeString(profile.systemPrompt));

    Value conversations = Value::makeArray();
    for (const StoredConversation& conversation : profile.conversations) {
        Value one = Value::makeObject();
        one.set("id", Value::makeInteger(conversation.id));
        one.set("title", Value::makeString(conversation.title));
        one.set("updatedAt", Value::makeString(conversation.updatedAt));

        Value messages = Value::makeArray();
        for (const StoredMessage& message : conversation.messages) {
            Value entry = Value::makeObject();
            entry.set("role", Value::makeString(message.role));
            entry.set("content", Value::makeString(message.content));
            messages.push(std::move(entry));
        }
        one.set("messages", std::move(messages));
        conversations.push(std::move(one));
    }
    document.set("conversations", std::move(conversations));

    document.set("recallEnabled", Value::makeBoolean(profile.recallEnabled));
    Value memories = Value::makeArray();
    for (const StoredMemory& memory : profile.memories) {
        Value entry = Value::makeObject();
        entry.set("id", Value::makeInteger(memory.id));
        entry.set("kind", Value::makeString(memory.kind));
        entry.set("text", Value::makeString(memory.text));
        entry.set("createdAt", Value::makeString(memory.createdAt));
        entry.set("updatedAt", Value::makeString(memory.updatedAt.empty() ? memory.createdAt : memory.updatedAt));
        entry.set("source", Value::makeString(memory.source));
        entry.set("sensitive", Value::makeBoolean(memory.sensitive));
        memories.push(std::move(entry));
    }
    document.set("memories", std::move(memories));
    Value examples = Value::makeArray();
    for (const StoredTrainingExample& example : profile.trainingExamples) {
        Value entry = Value::makeObject();
        entry.set("id", Value::makeInteger(example.id));
        entry.set("prompt", Value::makeString(example.prompt));
        entry.set("rejected", Value::makeString(example.rejected));
        entry.set("preferred", Value::makeString(example.preferred));
        entry.set("createdAt", Value::makeString(example.createdAt));
        examples.push(std::move(entry));
    }
    document.set("trainingExamples", std::move(examples));
    return document;
}

// The first line of whatever the user said, trimmed to something a list can
// show. An assistant's first message is used only when there is no user one,
// so an empty conversation is titled from what is actually in it.
std::string deriveTitle(const StoredConversation& conversation) {
    for (const StoredMessage& message : conversation.messages) {
        if (message.role != "user" && message.role != "assistant") {
            continue;
        }
        std::string title = message.content;
        const std::size_t newline = title.find('\n');
        if (newline != std::string::npos) {
            title.erase(newline);
        }
        while (!title.empty() && (title.front() == ' ' || title.front() == '\t')) {
            title.erase(title.begin());
        }
        while (!title.empty() && (title.back() == ' ' || title.back() == '\t' ||
                                  title.back() == '\r')) {
            title.pop_back();
        }
        if (title.size() > kTitleLength) {
            // Cut on a byte boundary that is also a character boundary. Without
            // this, a title ending mid-emoji is a title that no longer parses.
            std::size_t cut = kTitleLength;
            while (cut > 0 && (static_cast<unsigned char>(title[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            title.resize(cut);
        }
        return title;
    }
    return {};
}

// Reads a field that must be text when it is there. Absent is not an error:
// these three were all added after the first version of the format, and a
// document written by an earlier Kestrel is not a broken one.
bool readOptionalString(const Value& body, std::string_view key, std::string& out,
                        std::string& error) {
    const Value* field = body.find(key);
    if (field == nullptr) {
        return true;
    }
    const auto text = field->asString();
    if (!text.has_value()) {
        error = "the profile's \"" + std::string(key) + "\" is not text";
        return false;
    }
    out = std::string(*text);
    return true;
}

// Recall was added in version 2. A version 1 document has none of these fields,
// which is a document with recall off and nothing remembered, not a broken one.
bool readRecall(const Value& body, Profile& profile, std::string& error) {
    const Value* enabled = body.find("recallEnabled");
    if (enabled != nullptr) {
        const auto flag = enabled->asBoolean();
        if (!flag.has_value()) {
            error = "the profile's \"recallEnabled\" is not true or false";
            return false;
        }
        profile.recallEnabled = *flag;
    }
    const Value* memories = body.find("memories");
    if (memories == nullptr) {
        return true;
    }
    const std::vector<json::Value>* list = memories->asArray();
    if (list == nullptr) {
        error = "the profile's memories are not a list";
        return false;
    }
    if (list->size() > 2000) {
        error = "the profile exceeds the 2000-record memory limit; use a compatible build to manage it";
        return false;
    }
    std::unordered_set<std::int64_t> ids;
    for (const json::Value& entry : *list) {
        if (!entry.isObject()) {
            error = "a memory in the profile is not a record";
            return false;
        }
        StoredMemory memory;
        const Value* id = entry.find("id");
        if (id != nullptr) {
            const auto number = id->asInteger();
            if (!number.has_value()) {
                error = "a memory in the profile has an id that is not a number";
                return false;
            }
            memory.id = *number;
        }
        if (!readOptionalString(entry, "kind", memory.kind, error) ||
            !readOptionalString(entry, "text", memory.text, error) ||
            !readOptionalString(entry, "createdAt", memory.createdAt, error) ||
            !readOptionalString(entry, "updatedAt", memory.updatedAt, error) ||
            !readOptionalString(entry, "source", memory.source, error)) {
            return false;
        }
        if (const Value* sensitivity = entry.find("sensitive")) {
            const auto flag = sensitivity->asBoolean();
            if (!flag) { error = "a memory sensitivity flag is not true or false"; return false; }
            memory.sensitive = *flag;
        }
        if (memory.id <= 0 || memory.id == std::numeric_limits<std::int64_t>::max()
            || !ids.insert(memory.id).second || memory.text.empty() || memory.text.size() > 500
            || memory.source.size() > 200 || memory.createdAt.size() > 64 || memory.updatedAt.size() > 64
            || (memory.kind != "fact" && memory.kind != "summary" && memory.kind != "episode")) {
            error = "a memory has an invalid or duplicate ID, kind, text, timestamp, or source; nothing was loaded";
            return false;
        }
        if (memory.updatedAt.empty()) { memory.updatedAt = memory.createdAt; }
        profile.memories.push_back(std::move(memory));
    }
    return true;
}

bool readTrainingExamples(const Value& body, Profile& profile, std::string& error) {
    const Value* field = body.find("trainingExamples");
    if (field == nullptr) { return true; }
    const auto* list = field->asArray();
    if (list == nullptr || list->size() > 1000) {
        error = "trainingExamples must be a list of at most 1000 reviewed examples";
        return false;
    }
    std::unordered_set<std::int64_t> ids;
    for (const Value& entry : *list) {
        StoredTrainingExample example;
        const Value* id = entry.find("id");
        const auto number = id != nullptr ? id->asInteger() : std::nullopt;
        if (!entry.isObject() || !number || *number <= 0
            || *number == std::numeric_limits<std::int64_t>::max() || !ids.insert(*number).second) {
            error = "a reviewed training example has an invalid or duplicate ID";
            return false;
        }
        example.id = *number;
        if (!readOptionalString(entry, "prompt", example.prompt, error)
            || !readOptionalString(entry, "rejected", example.rejected, error)
            || !readOptionalString(entry, "preferred", example.preferred, error)
            || !readOptionalString(entry, "createdAt", example.createdAt, error)) { return false; }
        if (example.prompt.empty() || example.preferred.empty() || example.prompt.size() > 4000
            || example.rejected.size() > 4000 || example.preferred.size() > 4000
            || example.createdAt.size() > 64) {
            error = "a reviewed training example has empty or oversized text";
            return false;
        }
        profile.trainingExamples.push_back(std::move(example));
    }
    return true;
}

} // namespace

std::string Profile::checksumOf(const std::string& canonicalBody) {
    return toHex(Sha256::hash(canonicalBody));
}

std::string Profile::bodyJson() const {
    return bodyValue(*this).serialize();
}

int Profile::upsertConversation(StoredConversation conversation) {
    if (conversation.title.empty()) {
        conversation.title = deriveTitle(conversation);
    }
    for (StoredConversation& existing : conversations) {
        if (existing.id == conversation.id) {
            existing = std::move(conversation);
            return existing.id;
        }
    }
    conversations.push_back(std::move(conversation));
    return conversations.back().id;
}

std::string Profile::toJson() const {
    Value document = bodyValue(*this);
    // The checksum is taken over the body before the checksum member is added.
    // Written as a separate statement because the order matters and an argument
    // evaluated inside the call would make it a matter of evaluation order
    // rather than a matter of reading.
    const std::string body = document.serialize();
    document.set("checksum", Value::makeString(checksumOf(body)));
    // Indented, because this is a file a person is meant to open and ask what
    // the assistant thinks it knows.
    return document.serializeIndented(2);
}

bool Profile::fromJson(std::string_view document, Profile& out, std::string& error) {
    // Cleared first and never part-way through. A caller that ignores the
    // return value must end up with no profile, not with half of one that looks
    // like a user who deleted some of their own history.
    out = Profile{};

    std::string parseError;
    const auto parsed = json::Value::parse(document, parseError);
    if (!parsed.has_value()) {
        error = "the profile could not be read: " + parseError;
        return false;
    }
    if (!parsed->isObject()) {
        error = "the profile is not a document";
        return false;
    }

    const Value* stated = parsed->find("checksum");
    if (stated == nullptr) {
        error = "the profile carries no checksum, so there is no way to tell whether it is intact";
        return false;
    }
    const auto statedText = stated->asString();
    if (!statedText.has_value()) {
        error = "the profile's checksum is not text";
        return false;
    }

    Value body = *parsed;
    body.erase("checksum");
    // One comparison, and it is the whole of the integrity check: the body as
    // it was re-serialised from what was read, against the body as it was when
    // the checksum was taken. Anything that edited, truncated or re-encoded the
    // document in between lands here.
    if (checksumOf(body.serialize()) != *statedText) {
        error = "the profile does not match its checksum. It has been altered or "
                "truncated since it was written, so it has not been loaded.";
        return false;
    }

    const Value* version = body.find("version");
    if (version == nullptr) {
        error = "the profile carries no version, so this build cannot know how to read it";
        return false;
    }
    const auto versionNumber = version->asInteger();
    if (!versionNumber.has_value()) {
        error = "the profile's version is not a number";
        return false;
    }
    if (*versionNumber < 1) {
        error = "the profile version must be positive";
        return false;
    }
    if (*versionNumber > kCurrentVersion) {
        // Refused rather than best-effort. A newer field that is quietly dropped
        // is a user losing something they can plainly see in the file.
        error = "the profile was written by a newer Kestrel (version " +
                std::to_string(*versionNumber) + ", this build reads " +
                std::to_string(kCurrentVersion) + "), so it has not been loaded";
        return false;
    }

    // Built here and moved across only at the end, for every field. Writing
    // straight into `out` would leave the fields that parsed before one that did
    // not sitting in a profile the caller was told is empty -- which is the
    // whole failure this function promises not to do.
    Profile assembled;
    assembled.loadedVersion = *versionNumber;
    if (!readOptionalString(body, "modelPath", assembled.modelPath, error) ||
        !readOptionalString(body, "systemPrompt", assembled.systemPrompt, error) ||
        !readRecall(body, assembled, error) ||
        !readTrainingExamples(body, assembled, error)) {
        return false;
    }

    const Value* conversations = body.find("conversations");
    if (conversations == nullptr) {
        out = std::move(assembled);
        return true;
    }
    const std::vector<json::Value>* list = conversations->asArray();
    if (list == nullptr) {
        error = "the profile's conversations are not a list";
        return false;
    }
    for (const json::Value& entry : *list) {
        if (!entry.isObject()) {
            error = "a conversation in the profile is not a record";
            return false;
        }
        StoredConversation conversation;
        const Value* id = entry.find("id");
        if (id != nullptr) {
            const auto number = id->asInteger();
            if (!number.has_value()) {
                error = "a conversation in the profile has an id that is not a number";
                return false;
            }
            // The id is stored in an int, so a 64-bit value that does not fit is
            // a document to refuse rather than a number to truncate: a silent
            // truncation would give the conversation a different id from the one
            // in the file, and the next save would write that different id back.
            if (*number < std::numeric_limits<int>::min() ||
                *number > std::numeric_limits<int>::max()) {
                error = "a conversation in the profile has an id that does not fit";
                return false;
            }
            conversation.id = static_cast<int>(*number);
        }
        if (!readOptionalString(entry, "title", conversation.title, error) ||
            !readOptionalString(entry, "updatedAt", conversation.updatedAt, error)) {
            return false;
        }
        const Value* messages = entry.find("messages");
        if (messages != nullptr) {
            const std::vector<json::Value>* storedMessages = messages->asArray();
            if (storedMessages == nullptr) {
                error = "the messages of a conversation in the profile are not a list";
                return false;
            }
            for (const json::Value& stored : *storedMessages) {
                if (!stored.isObject()) {
                    error = "a message in the profile is not a record";
                    return false;
                }
                StoredMessage message;
                if (!readOptionalString(stored, "role", message.role, error) ||
                    !readOptionalString(stored, "content", message.content, error)) {
                    return false;
                }
                conversation.messages.push_back(std::move(message));
            }
        }
        assembled.conversations.push_back(std::move(conversation));
    }
    out = std::move(assembled);
    return true;
}

bool Profile::load(SecretStore& store, std::string_view account, std::string_view service,
                   Profile& out, std::string& error) {
    out = Profile{};
    // Cleared here rather than left to the store, because whether a miss counts
    // as success is decided below by asking whether this is empty -- and a
    // caller that reuses one error string across calls must not be able to turn
    // a first run into a failure by having something in it.
    error.clear();
    const auto sealed = store.sealed(account, service, error);
    if (!sealed.has_value()) {
        // The SecretStore contract is that a missing account leaves `error`
        // empty and an unreadable one fills it, so this is the whole of the
        // difference between "first run" and "something is wrong".
        return error.empty();
    }
    const std::string document(sealed->begin(), sealed->end());
    return fromJson(document, out, error);
}

bool Profile::save(SecretStore& store, std::string_view account, std::string_view service,
                   std::string& error) const {
    const std::string document = toJson();
    const std::vector<std::uint8_t> bytes(document.begin(), document.end());
    return store.seal(account, service, bytes, error);
}

bool Profile::forget(SecretStore& store, std::string_view account, std::string_view service,
                     std::string& error) {
    return store.forget(account, service, error);
}

} // namespace kestrel::storage
