#include "storage/secretstore.h"

#include "storage/secretsearch.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace kestrel::storage {
namespace {

// Whether secret-tool is on PATH. Asked of the shell once and cached, because
// the answer cannot change while Kestrel is up and spawning a shell to find out
// is not free.
//
// This is deliberately not a function of an arbitrary program name: the only
// caller wants one answer, and a cache keyed on nothing but a function-local
// static is how a "find secret-tool" helper ends up answering "yes" for every
// program it is later asked about.
bool haveSecretTool() {
    static const bool present = [] {
        return std::system("command -v secret-tool >/dev/null 2>&1") == 0;
    }();
    return present;
}

std::string shellQuote(const std::string& value) {
    // Single quotes, with the one character that cannot appear inside them
    // handled by closing, escaping and reopening. Service and account names are
    // ours, but a name that reached here from a file should not be able to
    // become a command.
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(character);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

// Runs a secret-tool command, collects everything it said with standard error
// folded in, and returns its exit status.
//
// The fold is what makes the result readable at all. libsecret uses exit 1 for
// two entirely different things: in secret_tool_action_lookup a miss returns 1
// having printed nothing, while a real failure returns 1 after printing the
// GError message. Nothing in the status alone tells them apart, and the
// difference is the difference between a first run and a keyring that will not
// open.
int runCapturing(const std::string& command, std::string& output) {
    const std::string withDiagnostics = command + " 2>&1";
    std::FILE* pipe = ::popen(withDiagnostics.c_str(), "r");
    if (pipe == nullptr) {
        return -1;
    }
    std::array<char, 512> chunk{};
    output.clear();
    while (std::fgets(chunk.data(), static_cast<int>(chunk.size()), pipe) != nullptr) {
        output.append(chunk.data());
    }
    return ::pclose(pipe);
}

std::string trimmed(const std::string& text) {
    std::string result = text;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

std::string toHex(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        text.push_back(kDigits[(byte >> 4) & 0x0f]);
        text.push_back(kDigits[byte & 0x0f]);
    }
    return text;
}

std::optional<std::vector<std::uint8_t>> fromHex(const std::string& text) {
    if (text.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 2);
    const auto value = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = value(text[i]);
        const int low = value(text[i + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return bytes;
}

// The attribute pairs every subcommand below is asked about, quoted once, so
// that store, clear and search cannot drift onto asking about different items.
std::string attributesFor(std::string_view account, std::string_view service) {
    return " service " + shellQuote(std::string(service)) +
           " account " + shellQuote(std::string(account));
}

// The `search --unlock` command for one set of attributes, and nothing else:
// the two callers below differ in what they do with the answer, never in how
// the question is asked. `lookup` cannot be used for either -- it exits 1 for a
// miss and for a failure alike, printing a reason only in the second case, so
// any caller has to infer "nothing is stored" from silence. --unlock is what
// makes `search` safe here: without it, locked items are skipped, and a locked
// keyring prints nothing and looks exactly like a miss.
std::string searchCommand(const std::string& attributes) {
    return "secret-tool search --unlock" + attributes;
}

// The three operations below are free functions rather than members for a
// reason that is not stylistic: available() is const, and it has to perform a
// real write and a real delete to find out whether a locked keyring will
// accept anything. A const member cannot call a non-const member, so a probe
// implemented in terms of seal() and forget() does not compile. This is the
// same shape the Windows backend uses for its probe.
bool sealViaTool(std::string_view account, std::string_view service,
                 const std::vector<std::uint8_t>& blob, std::string& error) {
    // The secret goes on standard input. Passing it as an argument would put
    // the user's profile in the process list, where every other process on
    // the machine can read it.
    const std::string command =
        "secret-tool store --label='Kestrel profile'" + attributesFor(account, service);
    std::FILE* pipe = ::popen(command.c_str(), "w");
    if (pipe == nullptr) {
        error = "could not run secret-tool";
        return false;
    }
    // Hex, and hex only: libsecret refuses to store a secret that is not valid
    // UTF-8, and it reads standard input to end of file, so the newline below
    // is stored with the digits and stripped again on the way back.
    const std::string hex = toHex(blob);
    std::fwrite(hex.data(), 1, hex.size(), pipe);
    std::fputc('\n', pipe);
    if (::pclose(pipe) != 0) {
        error = "secret-tool would not store the profile; is the keyring unlocked?";
        return false;
    }
    return true;
}

std::optional<std::vector<std::uint8_t>> readViaTool(std::string_view account,
                                                     std::string_view service,
                                                     std::string& error) {
    // `search --unlock` rather than `lookup`: the two report a keyring that will
    // not open differently, and only `search` can say which of the two things
    // happened. The classification of what came back lives in
    // classifySecretSearch, because "what does this report mean" is the part
    // worth getting right and the part worth testing without a keyring.
    const std::string attributes = attributesFor(account, service);
    std::string output;
    const int status = runCapturing(searchCommand(attributes), output);
    if (status < 0) {
        error = "could not run secret-tool to read the profile";
        return std::nullopt;
    }

    const SecretSearchReport report = classifySecretSearch(status, output);
    if (report.outcome == SecretSearchOutcome::readable) {
        auto bytes = fromHex(report.secret);
        if (!bytes.has_value()) {
            error = "the keyring returned something that is not a sealed profile";
            return std::nullopt;
        }
        return bytes;
    }
    if (report.outcome == SecretSearchOutcome::absent) {
        // Nothing matched, which libsecret says positively rather than by saying
        // nothing. A miss is a first run and not a failure, so the error stays
        // empty: that empty string is the whole of the SecretStore contract for
        // "nothing stored", and Profile::load reads it that way on purpose.
        return std::nullopt;
    }
    // Unreadable or failed, and both must report one. An empty error here is
    // what lets the caller treat a profile it could not read as one that was
    // never there, which is the save that overwrites it.
    error = report.outcome == SecretSearchOutcome::unreadable
                ? "a profile is stored in the keyring, but the keyring would not "
                  "release it: " + report.reason
                : "the keyring would not release the profile: " + report.reason;
    return std::nullopt;
}

bool forgetViaTool(std::string_view account, std::string_view service, std::string& error) {
    const std::string attributes = attributesFor(account, service);
    std::string output;
    if (runCapturing("secret-tool clear" + attributes, output) != 0) {
        // Reporting "forgotten" when the keyring refused would be the one lie
        // this store must not tell, because the profile is still in there.
        const std::string said = trimmed(output);
        error = "the keyring would not forget the profile: " +
                (said.empty() ? std::string("secret-tool failed without saying why") : said);
        return false;
    }

    // A zero status from `clear` is not evidence that anything was removed. The
    // manual says it removes "all unlocked items that match", so against a
    // locked keyring it removes nothing, has nothing to complain about, and
    // exits 0 -- and the caller would then tell a user they are forgotten while
    // their conversation is still sitting in the keyring.
    //
    // So the claim is checked rather than inferred. Asking the same question
    // again through `search --unlock` is what makes the difference between "I
    // removed it" and "nothing matching is there any more", and a miss answers
    // the second just as well as the first did: there was nothing to remove,
    // which is the state the caller asked for.
    std::string report;
    const int status = runCapturing(searchCommand(attributes), report);
    const SecretSearchReport outcome = classifySecretSearch(status, report);
    if (outcome.outcome == SecretSearchOutcome::absent) {
        return true;
    }
    if (outcome.outcome == SecretSearchOutcome::failed) {
        error = "the keyring accepted the request but would not say whether the "
                "profile was removed: " + outcome.reason;
        return false;
    }
    if (outcome.outcome == SecretSearchOutcome::unreadable) {
        error = "the keyring still holds the profile and would not release it, so "
                "nothing was removed: " + outcome.reason;
        return false;
    }
    // Readable and still there: the item survived a clear that reported success,
    // which is a keyring behaving in a way this store cannot account for.
    error = "the keyring accepted the request but the profile is still there";
    return false;
}

class LinuxSecretStore final : public SecretStore {
public:
    std::string description() const override {
        return "GNOME keyring / KDE Wallet via secret-tool";
    }

    bool available(std::string& unavailableReason) const override {
        if (!haveSecretTool()) {
            unavailableReason =
                "no keyring service was found. Install libsecret (which provides secret-tool) "
                "and unlock your keyring, or Kestrel will not save anything.";
            return false;
        }
        // Present is not the same as usable: secret-tool exits non-zero with no
        // message when the keyring is locked or no keyring daemon is running,
        // which is the state a freshly rebooted machine is in.
        std::string error;
        if (!sealViaTool("probe", "com.kestrel.probe", {'p', 'r', 'o', 'b', 'e'}, error)) {
            unavailableReason = "the keyring did not accept a test value: " + error;
            return false;
        }
        // The probe has to come back out again, and a keyring that will accept
        // a write but refuse a delete is not a keyring this store can use: the
        // probe would sit there for the life of the process, and "forget
        // everything" would leave it behind.
        std::string cleanupError;
        if (!forgetViaTool("probe", "com.kestrel.probe", cleanupError)) {
            unavailableReason =
                "the keyring accepted the test value but would not remove it: " + cleanupError;
            return false;
        }
        return true;
    }

    bool seal(std::string_view account, std::string_view service,
              const std::vector<std::uint8_t>& blob, std::string& error) override {
        return sealViaTool(account, service, blob, error);
    }

    std::optional<std::vector<std::uint8_t>> sealed(std::string_view account,
                                                    std::string_view service,
                                                    std::string& error) override {
        return readViaTool(account, service, error);
    }

    bool forget(std::string_view account, std::string_view service, std::string& error) override {
        return forgetViaTool(account, service, error);
    }
};

} // namespace

std::unique_ptr<SecretStore> makePlatformSecretStore() {
    return std::make_unique<LinuxSecretStore>();
}

} // namespace kestrel::storage
