#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "storage/secretstore.h"

namespace kestrel::storage {

// One message as it is kept between sessions.
struct StoredMessage {
    // "user", "assistant" or "system". A string rather than an enum because it
    // is a document field, and a document has to be readable by something that
    // is not this program.
    std::string role;
    std::string content;
};

// One conversation as it is kept between sessions.
struct StoredConversation {
    int id = 0;
    // The first thing the user said, or a truncation of it. Derived rather than
    // stored twice, so it cannot drift from the message it came from.
    std::string title;
    // ISO-8601, UTC. Kept as text because that is what it is for: a person
    // reading the document.
    std::string updatedAt;
    std::vector<StoredMessage> messages;
};

// Everything Kestrel knows about you, in one document.
//
// This is the top of the storage layer: the keystore holds opaque sealed bytes,
// and this decides what those bytes mean. Everything above it -- the model
// path, the system prompt, the conversation history -- is a field here, which
// is what makes "what does Kestrel know about me" a question with an answer.
//
// Two decisions worth stating outright.
//
// The document carries its own checksum. DPAPI and the macOS keychain already
// authenticate the bytes they hand back, so on those platforms the checksum
// proves nothing extra. It earns its place in the two cases they do not cover:
// a document held by InMemorySecretStore, and a document that has been
// truncated or hand-edited. A profile that loads half of itself is worse than
// one that refuses to load, because a half-loaded profile looks exactly like a
// user who deleted some of their history on purpose.
//
// The version is checked on the way in and not on the way out. A document
// written by a newer Kestrel is refused rather than guessed at, because the
// alternative is a newer field being silently dropped and a user quietly losing
// something they can see in the file.
class Profile {
public:
    // Bumped only alongside a migration that can read the previous version.
    static constexpr std::int64_t kCurrentVersion = 1;

    // The name the document is filed under inside the store. Distinct from
    // kProfileService so a future second secret cannot land on top of this one.
    static constexpr std::string_view kProfileAccount = "profile";

    // Where the model was. Empty means none has been chosen, which is the state
    // a first run is in and not an error.
    std::string modelPath;

    // What the assistant is told about itself and the user.
    std::string systemPrompt;

    std::vector<StoredConversation> conversations;

    [[nodiscard]] bool empty() const {
        return modelPath.empty() && systemPrompt.empty() && conversations.empty();
    }

    // Adds a conversation, or replaces the one with the same id, and returns its
    // id. Replacement is by id rather than append because a save that arrives
    // twice must not double the history.
    int upsertConversation(StoredConversation conversation);

    // Reads the document out of the store.
    //
    // Nothing stored is a first run, not a failure: `out` is left empty and the
    // call succeeds. Anything else -- a blob that will not parse, a checksum
    // that does not match, a version from the future -- is a failure, and
    // `out` is left empty rather than partly filled. A caller that ignores the
    // return value therefore gets an empty profile, never a partial one.
    static bool load(SecretStore& store, std::string_view account, std::string_view service,
                     Profile& out, std::string& error);

    // Writes the document into the store, checksum included.
    [[nodiscard]] bool save(SecretStore& store, std::string_view account,
                            std::string_view service, std::string& error) const;

    // Removes the stored document. Removing one that was never there succeeds,
    // because "forget everything" pressed twice is not an error.
    static bool forget(SecretStore& store, std::string_view account, std::string_view service,
                       std::string& error);

    // The document as text, checksum included, for a file a person can open.
    // load() accepts exactly what this produces.
    [[nodiscard]] std::string toJson() const;

    // The same document parsed out of text. Separate from load() so the format
    // can be tested -- and a hand-written document accepted -- without a
    // keystore anywhere in sight.
    [[nodiscard]] static bool fromJson(std::string_view document, Profile& out, std::string& error);

    // The checksum of a body: SHA-256 over the canonical bytes of the object
    // with the checksum member taken off. Exposed because it is the one piece
    // of the format a test needs to state independently.
    [[nodiscard]] static std::string checksumOf(const std::string& canonicalBody);

private:
    // The document without its checksum, as a JSON object. The checksum is
    // never part of this: it is computed over the serialisation of exactly
    // these members, which is what makes it verifiable.
    [[nodiscard]] std::string bodyJson() const;
};

} // namespace kestrel::storage
