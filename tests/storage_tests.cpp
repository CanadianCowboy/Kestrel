#include "storage/atomicfile.h"
#include "storage/paths.h"
#include "storage/secretstore.h"
#include "storage/sha256.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace kestrel;

namespace fs = std::filesystem;

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        return;
    }
    std::cout << "  FAIL " << what << "\n";
    ++g_failures;
}

std::string hexOf(const std::array<std::uint8_t, storage::Sha256::kDigestBytes>& digest) {
    return storage::toHex(digest);
}

// A scratch directory that removes itself, so a failing assert cannot leave a
// profile lying around in the user's real data directory.
class ScratchDirectory {
public:
    explicit ScratchDirectory(const std::string& name) {
        const fs::path base = fs::temp_directory_path() / ("kestrel-storage-" + name);
        std::error_code code;
        fs::remove_all(base, code);
        fs::create_directories(base, code);
        m_path = base;
    }

    ~ScratchDirectory() {
        std::error_code code;
        fs::remove_all(m_path, code);
    }

    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    [[nodiscard]] const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

void testSha256KnownAnswers() {
    // The published vectors. A hash that is subtly wrong -- wrong padding, wrong
    // length encoding, wrong byte order -- passes every round trip it is part
    // of, because both ends are equally wrong. Only a fixed vector catches it.
    check(hexOf(storage::Sha256::hash("")) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA-256 of the empty string");
    check(hexOf(storage::Sha256::hash("abc")) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 of \"abc\"");
    check(hexOf(storage::Sha256::hash(
              "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "SHA-256 of the 56-byte vector, which crosses a block boundary");
    check(hexOf(storage::Sha256::hash(
              "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")) ==
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1",
          "SHA-256 of the 112-byte vector, which crosses two");

    // The same bytes fed in pieces must give the same answer as one call, or
    // the streaming path used on a large file is not the one that was tested.
    storage::Sha256 streamed;
    streamed.update("a");
    streamed.update("bc");
    check(hexOf(streamed.finish()) == hexOf(storage::Sha256::hash("abc")),
          "SHA-256 is the same whether fed in pieces or at once");

    // Exactly one block, and exactly one byte either side of a block, since
    // those are the sizes where the padding arithmetic goes wrong.
    for (const std::size_t size : {55u, 56u, 57u, 63u, 64u, 65u, 119u, 120u}) {
        const std::string data(size, 'x');
        storage::Sha256 chunked;
        for (std::size_t offset = 0; offset < size; offset += 7) {
            chunked.update(data.substr(offset, 7));
        }
        check(hexOf(chunked.finish()) == hexOf(storage::Sha256::hash(data)),
              "SHA-256 across block boundaries at size " + std::to_string(size));
    }
}

void testAtomicWriteReplacesAndLeavesNoScratch() {
    const ScratchDirectory scratch("atomic");
    const fs::path target = scratch.path() / "profile.bin";
    std::string error;

    check(storage::writeFileAtomically(target, "first", error), "a first write succeeds: " + error);
    std::vector<std::uint8_t> read;
    check(storage::readWholeFile(target, read, error), "the file reads back: " + error);
    check(std::string(read.begin(), read.end()) == "first", "the first write is what is on disk");

    // The overwrite is the case that matters: this runs on every save.
    check(storage::writeFileAtomically(target, "second, which is longer", error),
          "an overwrite succeeds: " + error);
    check(storage::readWholeFile(target, read, error), "the overwritten file reads back: " + error);
    check(std::string(read.begin(), read.end()) == "second, which is longer",
          "the overwrite is what is on disk");

    // A reader must never see a half-written file, and the scratch file must
    // not survive: a leftover "profile.bin.tmp" is the visible symptom of the
    // window this function exists to close.
    check(!fs::exists(target.string() + ".tmp"), "no scratch file is left behind");

    // A missing file is not an error, because a first run has one.
    std::vector<std::uint8_t> absent;
    check(storage::readWholeFile(scratch.path() / "nothing-here", absent, error),
          "reading a file that does not exist is not an error");
    check(absent.empty(), "a missing file reads as no bytes");
}

void testInMemoryStoreContract() {
    storage::InMemorySecretStore store;
    const std::vector<std::uint8_t> first{'a', 'b', 'c'};
    const std::vector<std::uint8_t> second{'d', 'e', 'f', 'g'};
    std::string error;

    check(store.available(error), "the in-memory store is available: " + error);
    check(store.description().find("in-memory") != std::string::npos,
          "the in-memory store says plainly that it is not encryption");

    // Nothing stored yet is a first run, not a failure.
    check(!store.sealed("profile", storage::kProfileService, error).has_value(),
          "an empty store has no blob");

    check(store.seal("profile", storage::kProfileService, first, error), "a first seal: " + error);
    const auto read = store.sealed("profile", storage::kProfileService, error);
    check(read.has_value() && *read == first, "the blob comes back exactly");

    // Overwriting is the normal case: every save after the first.
    check(store.seal("profile", storage::kProfileService, second, error), "a second seal: " + error);
    const auto reread = store.sealed("profile", storage::kProfileService, error);
    check(reread.has_value() && *reread == second, "the second seal replaced the first");

    // Two accounts must not see each other.
    check(!store.sealed("other", storage::kProfileService, error).has_value(),
          "one account's blob is not another's");

    check(store.forget("profile", storage::kProfileService, error), "forget succeeds: " + error);
    check(!store.sealed("profile", storage::kProfileService, error).has_value(),
          "the blob is gone after forget");
    check(store.forget("profile", storage::kProfileService, error),
          "forgetting something already forgotten is still success");
}

void testPlatformStore() {
    const std::unique_ptr<storage::SecretStore> store = storage::makePlatformSecretStore();
    std::string error;

    const std::filesystem::path directory = storage::userDataDirectory();
    if (directory.empty()) {
        std::cout << "  note  no user data directory in this environment; "
                     "the platform store cannot be exercised\n";
        return;
    }
    check(!store->description().empty(), "the platform store describes itself");

    // A locked or absent keyring is a real state, and it has to be a sentence
    // the user can act on. What must never happen is a silent downgrade to an
    // unencrypted file, so a keystore that cannot be used is reported, not
    // worked around.
    if (!store->available(error)) {
        std::cout << "  note  platform store unavailable here: " << error << "\n";
        check(!error.empty(), "an unavailable store explains itself");
        return;
    }

    const std::vector<std::uint8_t> secret{'s', 'e', 'c', 'r', 'e', 't'};
    const std::vector<std::uint8_t> replacement{'s', 'e', 'c', 'r', 'e', 't', '!', '!'};
    const std::string account = "storage-tests";

    check(store->seal(account, storage::kProfileService, secret, error),
          "the platform store seals: " + error);
    const auto read = store->sealed(account, storage::kProfileService, error);
    check(read.has_value(), "the platform store returns a blob: " + error);
    if (read.has_value()) {
        check(*read == secret, "the platform store round-trips the exact bytes");
    }

    check(store->seal(account, storage::kProfileService, replacement, error),
          "the platform store overwrites: " + error);
    const auto reread = store->sealed(account, storage::kProfileService, error);
    check(reread.has_value() && *reread == replacement,
          "the platform store kept the replacement, not the original");

    // A blob sealed under one account must not open under another. On DPAPI
    // that is a different user; here it is a different account name, which is
    // the cheapest check that the binding is there at all.
    check(!store->sealed("someone-else", storage::kProfileService, error).has_value(),
          "one account's blob is not readable as another's");

    check(store->forget(account, storage::kProfileService, error), "forget succeeds: " + error);
    check(!store->sealed(account, storage::kProfileService, error).has_value(),
          "the platform store really forgot it");

    // The blob must not be the plaintext. On Windows this is a file under the
    // user's data directory, so the claim can be checked on the bytes rather
    // than taken on trust.
    check(store->seal(account, storage::kProfileService, secret, error),
          "a second seal for inspection: " + error);
    const fs::path sealed = storage::sealedBlobDirectory();
    if (!sealed.empty() && fs::exists(sealed)) {
        bool foundReadableCopy = false;
        for (const auto& entry : fs::directory_iterator(sealed)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            std::vector<std::uint8_t> bytes;
            std::string readError;
            if (!storage::readWholeFile(entry.path(), bytes, readError)) {
                continue;
            }
            const std::string text(bytes.begin(), bytes.end());
            if (text.find(std::string(secret.begin(), secret.end())) != std::string::npos) {
                foundReadableCopy = true;
            }
        }
        check(!foundReadableCopy, "no sealed file on disk contains the plaintext");
    }
    // Checked, not discarded: the test is the only thing asserting the store
    // can be emptied, and this line is the last chance to notice if it cannot.
    check(store->forget(account, storage::kProfileService, error),
          "the platform store can be emptied again: " + error);
}

} // namespace

int main() {
    std::cout << "storage tests\n";
    testSha256KnownAnswers();
    testAtomicWriteReplacesAndLeavesNoScratch();
    testInMemoryStoreContract();
    testPlatformStore();

    if (g_failures == 0) {
        std::cout << "storage tests passed\n";
        return 0;
    }
    std::cout << g_failures << " storage check(s) failed\n";
    return 1;
}
