#include "storage/profile.h"

#include "storage/secretstore.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

using kestrel::storage::InMemorySecretStore;
using kestrel::storage::kProfileService;
using kestrel::storage::Profile;
using kestrel::storage::StoredConversation;
using kestrel::storage::StoredMessage;

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        return;
    }
    std::cout << "  FAIL " << what << "\n";
    ++g_failures;
}

// A message containing an accented letter, an apostrophe, an emoji and a bell
// character. Every one of those has broken a JSON writer at some point, and the
// emoji is the one that breaks a truncating writer, because it is three bytes.
const char* kAwkwardText = "comment \xc3\xa9tait n'est pas ici \xf0\x9f\x9a\x80 and a \x07 bell";

StoredConversation conversationWith(int id, std::string userText) {
    StoredConversation conversation;
    conversation.id = id;
    conversation.updatedAt = "2026-09-26T12:00:00Z";
    conversation.messages.push_back(StoredMessage{"user", std::move(userText)});
    conversation.messages.push_back(StoredMessage{"assistant", "an answer, at length"});
    return conversation;
}

Profile aFullProfile() {
    Profile profile;
    profile.modelPath = "D:/kestrel-deps/models/qwen.gguf";
    profile.systemPrompt = "You are Kestrel. Be brief.";
    profile.upsertConversation(conversationWith(1, "what is the weather?"));
    profile.upsertConversation(conversationWith(2, "second conversation"));
    return profile;
}

void testRoundTripThroughTheStore() {
    InMemorySecretStore store;
    const Profile original = aFullProfile();

    std::string error;
    check(original.save(store, Profile::kProfileAccount, kProfileService, error),
          "a profile saves: " + error);

    Profile loaded;
    check(Profile::load(store, Profile::kProfileAccount, kProfileService, loaded, error),
          "and loads back: " + error);

    check(loaded.modelPath == original.modelPath, "the model path survives");
    check(loaded.systemPrompt == original.systemPrompt, "the system prompt survives");
    check(loaded.conversations.size() == 2, "both conversations survive");
    if (loaded.conversations.size() == 2) {
        check(loaded.conversations[0].id == 1 && loaded.conversations[1].id == 2,
              "and keep their order and ids");
        check(loaded.conversations[0].messages.size() == 2, "a message list survives");
        if (!loaded.conversations[0].messages.empty()) {
            check(loaded.conversations[0].messages[0].role == "user", "a role survives");
            check(loaded.conversations[0].messages[0].content == "what is the weather?",
                  "and the text of a message");
        }
    }
}

void testNothingStoredIsAFirstRun() {
    InMemorySecretStore store;
    Profile loaded;
    std::string error;
    // Deliberately pre-filled: a load must not leave this behind.
    loaded.modelPath = "left over from somewhere";
    check(Profile::load(store, Profile::kProfileAccount, kProfileService, loaded, error),
          "loading with nothing stored is not a failure");
    check(error.empty(), "and says nothing about it");
    check(loaded.empty(), "and the profile is empty");
    check(loaded.conversations.empty(), "with no conversations");
}

void testChecksumCatchesAnAlteredDocument() {
    InMemorySecretStore store;
    std::string error;
    Profile profile = aFullProfile();
    check(profile.save(store, Profile::kProfileAccount, kProfileService, error),
          "a profile saves: " + error);

    // Read the bytes back the way a bad disk or a curious program would, and
    // change one character of the body while leaving the checksum alone.
    auto sealed = store.sealed(Profile::kProfileAccount, kProfileService, error);
    check(sealed.has_value(), "the sealed bytes can be read: " + error);
    if (!sealed.has_value()) {
        return;
    }
    std::string document(sealed->begin(), sealed->end());
    const std::size_t at = document.find("what is the weather?");
    check(at != std::string::npos, "the altered text is in the document");
    if (at == std::string::npos) {
        return;
    }
    document[at + 1] = 'X';
    const std::vector<std::uint8_t> altered(document.begin(), document.end());
    check(store.seal(Profile::kProfileAccount, kProfileService, altered, error),
          "the altered document can be put back: " + error);

    Profile loaded;
    loaded.modelPath = "left over from somewhere";
    check(!Profile::load(store, Profile::kProfileAccount, kProfileService, loaded, error),
          "a document that has been altered does not load");
    check(error.find("checksum") != std::string::npos, "and the reason is the checksum: " + error);
    // The important half: a caller that ignores the return value must not end
    // up showing a history with a word quietly changed in it.
    check(loaded.empty(), "and the profile is left empty, not partly filled");
    check(loaded.modelPath.empty(), "with no leftovers from before the load");
}

void testATruncatedDocumentIsRefused() {
    InMemorySecretStore store;
    std::string error;
    Profile profile = aFullProfile();
    check(profile.save(store, Profile::kProfileAccount, kProfileService, error),
          "a profile saves: " + error);

    auto sealed = store.sealed(Profile::kProfileAccount, kProfileService, error);
    if (!sealed.has_value()) {
        check(false, "the sealed bytes can be read: " + error);
        return;
    }
    std::string document(sealed->begin(), sealed->end());
    document.resize(document.size() / 2);
    const std::vector<std::uint8_t> half(document.begin(), document.end());
    check(store.seal(Profile::kProfileAccount, kProfileService, half, error),
          "a truncated document can be put back");
    // Caught by the reader rather than by the checksum: halving a document
    // leaves unbalanced braces, so it never parses. The checksum earns its
    // keep in the test above, where the document is still perfectly valid JSON
    // and one character of it is wrong.

    Profile loaded;
    check(!Profile::load(store, Profile::kProfileAccount, kProfileService, loaded, error),
          "a truncated document does not load");
    check(loaded.empty(), "and leaves an empty profile");
}

void testDocumentWithoutAChecksumIsRefused() {
    Profile loaded;
    std::string error;
    check(!Profile::fromJson(R"({"version":1,"modelPath":"x"})", loaded, error),
          "a document with no checksum does not load");
    check(error.find("checksum") != std::string::npos, "and says so: " + error);
    check(loaded.empty(), "and leaves an empty profile");
}

void testVersionFromTheFutureIsRefused() {
    // A newer field dropped quietly is a user losing something they can plainly
    // see in the file, so this is refused rather than read best-effort.

    // Rebuild the body with a version of 9, take its checksum, and put the two
    // together. The checksum is correct, so what refuses this is the version and
    // not merely the integrity check.
    const std::string body =
        R"({"version":9,"modelPath":"D:/kestrel-deps/models/qwen.gguf",)"
        R"("systemPrompt":"You are Kestrel. Be brief.","conversations":[]})";
    const std::string document = std::string("{\n  \"version\": 9,\n") +
                                 "  \"modelPath\": \"D:/kestrel-deps/models/qwen.gguf\",\n" +
                                 "  \"systemPrompt\": \"You are Kestrel. Be brief.\",\n" +
                                 "  \"conversations\": [],\n" +
                                 "  \"checksum\": \"" + Profile::checksumOf(body) + "\"\n}";

    Profile loaded;
    std::string error;
    check(!Profile::fromJson(document, loaded, error), "a document from a newer Kestrel is refused");
    check(error.find("newer Kestrel") != std::string::npos, "and says so: " + error);
    check(loaded.empty(), "and leaves an empty profile");
}

void testAnOlderDocumentStillLoads() {
    // The fields added after the first version of the format are absent here,
    // and that is not a broken document: absence means the default.
    Profile loaded;
    std::string error;
    const std::string body = R"({"version":1})";
    const std::string document = std::string("{\n  \"version\": 1,\n") +
                                 "  \"checksum\": \"" + Profile::checksumOf(body) + "\"\n}";
    check(Profile::fromJson(document, loaded, error), "a minimal document loads: " + error);
    check(loaded.empty(), "and describes an empty profile");
    check(loaded.modelPath.empty(), "with no model chosen");
}

void testMalformedAndMistypedDocumentsAreRefused() {
    struct Case {
        const char* document;
        const char* because;
    };
    const std::vector<Case> cases = {
        {"not json at all", "text that is not a document"},
        {"[]", "a list where a document belongs"},
        {R"({"version":"one","checksum":"x"})", "a version that is not a number"},
        {R"({"checksum":"x"})", "no version at all"},
        {R"({"version":1,"checksum":5})", "a checksum that is not text"},
    };
    for (const Case& one : cases) {
        Profile loaded;
        loaded.modelPath = "left over";
        std::string error;
        check(!Profile::fromJson(one.document, loaded, error),
              std::string("refused: ") + one.because);
        check(!error.empty(), std::string("and says why: ") + one.because);
        check(loaded.empty(), std::string("and leaves nothing behind: ") + one.because);
    }
}

void testAFailurePartWayThroughLeavesNothing() {
    // The contract is that a failed load leaves an empty profile, and the easy
    // way to keep it is to clear the output on the way in. That is not enough:
    // a document whose first conversation is fine and whose second is not would
    // otherwise leave the first one behind. Both halves are built here by hand,
    // with correct checksums, so the failure is the document's shape and not
    // the integrity check.
    struct Case {
        const char* conversations;
        const char* because;
    };
    const std::vector<Case> cases = {
        {R"([{"id":1,"messages":[]},{"id":"two"}])", "a bad id in the second conversation"},
        {R"([{"id":1,"messages":[]},{"id":2,"messages":"nope"}])", "bad messages in the second"},
        {R"([{"id":1,"messages":[]},"not a record"])", "a second entry that is not a record"},
        {R"([{"id":1,"messages":[{"role":7}]}])", "a message with a mistyped field"},
    };
    for (const Case& one : cases) {
        const std::string body = std::string(R"({"version":1,"conversations":)") + one.conversations + "}";
        const std::string document = "{\n  \"version\": 1,\n  \"conversations\": " +
                                     std::string(one.conversations) + ",\n" +
                                     "  \"checksum\": \"" + Profile::checksumOf(body) + "\"\n}";

        Profile loaded;
        loaded.conversations.push_back(conversationWith(99, "left over from before"));
        loaded.modelPath = "left over too";
        std::string error;
        check(!Profile::fromJson(document, loaded, error), std::string("refused: ") + one.because);
        check(loaded.empty(),
              std::string("and nothing at all is left behind: ") + one.because +
                  " (had " + std::to_string(loaded.conversations.size()) + " conversations)");
    }
}

void testAnOutOfRangeIdIsRefused() {
    // The id is stored in an int. A 64-bit value that does not fit would be
    // truncated silently, and the next save would write the truncated id back,
    // so a round trip would quietly change the data.
    const std::string body =
        R"({"version":1,"conversations":[{"id":99999999999999,"messages":[]}]})";
    const std::string document = std::string("{\n  \"version\": 1,\n  \"conversations\": ") +
                                 R"([{"id":99999999999999,"messages":[]}],)" +
                                 "\n  \"checksum\": \"" + Profile::checksumOf(body) + "\"\n}";

    Profile loaded;
    std::string error;
    check(!Profile::fromJson(document, loaded, error), "an id that does not fit is refused");
    check(loaded.empty(), "and leaves an empty profile");
}

void testForgetEverything() {
    InMemorySecretStore store;
    std::string error;
    Profile profile = aFullProfile();
    check(profile.save(store, Profile::kProfileAccount, kProfileService, error),
          "a profile saves: " + error);

    check(Profile::forget(store, Profile::kProfileAccount, kProfileService, error),
          "forget succeeds: " + error);
    Profile loaded;
    check(Profile::load(store, Profile::kProfileAccount, kProfileService, loaded, error),
          "and what is left loads as a first run");
    check(loaded.empty(), "with nothing in it");

    // Pressing the button twice is not an error. It is the same button.
    check(Profile::forget(store, Profile::kProfileAccount, kProfileService, error),
          "forgetting something already forgotten is still success: " + error);
}

void testTitlesComeFromTheFirstThingSaid() {
    Profile profile;
    profile.upsertConversation(conversationWith(1, "  what is the weather?  "));
    check(profile.conversations.size() == 1, "a conversation is stored");
    if (!profile.conversations.empty()) {
        check(profile.conversations[0].title == "what is the weather?",
              "the title is the first thing said, trimmed, got: " + profile.conversations[0].title);
    }

    Profile multi;
    multi.upsertConversation(conversationWith(2, "first line\nsecond line"));
    if (!multi.conversations.empty()) {
        check(multi.conversations[0].title == "first line",
              "and only the first line of it, got: " + multi.conversations[0].title);
    }

    // A title is a preview, and a preview that cuts a character in half is a
    // document that no longer parses. The emoji has to land exactly on the cut
    // for this to mean anything: put it anywhere else and the boundary logic is
    // never reached, and the test passes for a reason that has nothing to do
    // with what it is checking. Fifty-eight ASCII bytes puts the three-byte
    // emoji at 58, 59 and 60, and the cut is at 60.
    Profile longOne;
    longOne.upsertConversation(conversationWith(3, std::string(58, 'x') + "\xf0\x9f\x9a\x80 tail"));
    if (!longOne.conversations.empty()) {
        const std::string& title = longOne.conversations[0].title;
        check(title.size() == 58,
              "a title cut at a character boundary keeps every whole character, got " +
                  std::to_string(title.size()) + " bytes");
        // Whatever the cut, it has to still be valid text.
        Profile reparsed;
        std::string reparseError;
        check(Profile::fromJson(longOne.toJson(), reparsed, reparseError),
              "a title cut at a character boundary is still valid text: " + reparseError);
    }

    Profile withoutMessages;
    StoredConversation nothing;
    nothing.id = 9;
    withoutMessages.upsertConversation(std::move(nothing));
    if (!withoutMessages.conversations.empty()) {
        check(withoutMessages.conversations[0].title.empty(),
              "a conversation with no messages has no title");
    }
}

void testUpsertReplacesRatherThanAppends() {
    // A save that arrives twice must not double somebody's history.
    Profile profile;
    StoredConversation first = conversationWith(1, "the first version");
    profile.upsertConversation(first);
    profile.upsertConversation(first);
    check(profile.conversations.size() == 1, "the same conversation twice is still one");

    profile.upsertConversation(conversationWith(1, "the second version"));
    check(profile.conversations.size() == 1, "and a revision replaces rather than adds");
    if (!profile.conversations.empty() && !profile.conversations[0].messages.empty()) {
        check(profile.conversations[0].messages[0].content == "the second version",
              "and the revision is the one that is kept");
        check(profile.conversations[0].title == "the second version",
              "including the derived title");
    }
}

void testAwkwardContentSurvives() {
    Profile profile;
    profile.modelPath = "C:\\models\\qwen \"small\".gguf";
    profile.systemPrompt = "Line one\nLine two\twith a tab, a \"quote\" and a backslash \\";
    profile.upsertConversation(conversationWith(1, kAwkwardText));

    Profile loaded;
    std::string error;
    check(Profile::fromJson(profile.toJson(), loaded, error), "an awkward document loads: " + error);
    check(loaded.modelPath == profile.modelPath, "a quoted path survives");
    check(loaded.systemPrompt == profile.systemPrompt, "a multi-line prompt survives");
    if (!loaded.conversations.empty() && !loaded.conversations[0].messages.empty()) {
        check(loaded.conversations[0].messages[0].content == kAwkwardText,
              "an awkward message survives, got: " + loaded.conversations[0].messages[0].content);
    }

    // And the document round-trips byte for byte, which is what the checksum
    // rests on: a document that re-serialises differently would fail its own
    // check the next time it was read.
    Profile again;
    check(Profile::fromJson(loaded.toJson(), again, error), "and again: " + error);
    check(again.toJson() == loaded.toJson(), "the document is stable across a round trip");
}

void testARealisticProfileSurvives() {
    // Ten conversations of four messages each, roughly a week of use, to catch
    // anything that only shows up at size.
    Profile profile;
    profile.modelPath = "D:/kestrel-deps/models/qwen.gguf";
    profile.systemPrompt = "You are Kestrel.";
    for (int id = 1; id <= 10; ++id) {
        StoredConversation conversation;
        conversation.id = id;
        conversation.updatedAt = "2026-09-26T12:00:00Z";
        for (int turn = 0; turn < 4; ++turn) {
            conversation.messages.push_back(
                StoredMessage{turn % 2 == 0 ? "user" : "assistant",
                              "message " + std::to_string(turn) + " of conversation " +
                                  std::to_string(id) + ", a little longer than the last one."});
        }
        profile.upsertConversation(std::move(conversation));
    }

    Profile loaded;
    std::string error;
    check(Profile::fromJson(profile.toJson(), loaded, error), "a realistic profile loads: " + error);
    check(loaded.conversations.size() == 10, "every conversation survives");
    check(loaded.toJson() == profile.toJson(), "and the document is unchanged");
}

} // namespace

int main() {
    std::cout << "profile tests\n";
    testRoundTripThroughTheStore();
    testNothingStoredIsAFirstRun();
    testChecksumCatchesAnAlteredDocument();
    testATruncatedDocumentIsRefused();
    testDocumentWithoutAChecksumIsRefused();
    testVersionFromTheFutureIsRefused();
    testAnOlderDocumentStillLoads();
    testMalformedAndMistypedDocumentsAreRefused();
    testAFailurePartWayThroughLeavesNothing();
    testAnOutOfRangeIdIsRefused();
    testForgetEverything();
    testTitlesComeFromTheFirstThingSaid();
    testUpsertReplacesRatherThanAppends();
    testAwkwardContentSurvives();
    testARealisticProfileSurvives();

    if (g_failures == 0) {
        std::cout << "profile tests passed\n";
        return 0;
    }
    std::cout << g_failures << " profile check(s) failed\n";
    return 1;
}
