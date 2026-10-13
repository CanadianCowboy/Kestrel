#include "storage/secretstore.h"

#include "storage/secretsearch.h"
#include "storage/secrettoolprocess_linux.h"

#include <chrono>
#include <unistd.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kestrel::storage {
namespace {

constexpr auto kKeyringTimeout = std::chrono::seconds(5);

SecretToolResult run(const std::vector<std::string>& arguments, std::string_view input = {}) {
    return runSecretTool(arguments, input, kKeyringTimeout);
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

// The literal attribute pairs every subcommand below is asked about, so
// that store, clear and search cannot drift onto asking about different items.
std::vector<std::string> commandFor(std::string action, std::string_view account,
                                    std::string_view service) {
    std::vector<std::string> arguments{std::move(action)};
    if (arguments.front() == "search") { arguments.emplace_back("--unlock"); }
    if (arguments.front() == "store") { arguments.emplace_back("--label=Kestrel profile"); }
    arguments.insert(arguments.end(), {"service", std::string(service), "account", std::string(account)});
    return arguments;
}

// The `search --unlock` command for one set of attributes, and nothing else:
// the two callers below differ in what they do with the answer, never in how
// the question is asked. `lookup` cannot be used for either -- it exits 1 for a
// miss and for a failure alike, printing a reason only in the second case, so
// any caller has to infer "nothing is stored" from silence. --unlock is what
// makes `search` safe here: without it, locked items are skipped, and a locked
// keyring prints nothing and looks exactly like a miss.
SecretSearchReport search(std::string_view account, std::string_view service) {
    const SecretToolResult result = run(commandFor("search", account, service));
    if (!result.error.empty()) {
        return {SecretSearchOutcome::failed, {}, result.error};
    }
    // Stderr must never be parsed as a secret. Even exit 0 with warnings is not
    // evidence of an absent profile; refuse instead of risking an overwrite.
    if (result.status != 0 || stderrReportsFailure(result.diagnostics)) {
        return {SecretSearchOutcome::failed, {},
                "secret-tool could not read the profile. Unlock the keyring and check the desktop D-Bus session."};
    }
    SecretSearchReport report = classifySecretSearch(result.status, result.output);
    if (report.outcome == SecretSearchOutcome::unreadable) {
        report.reason = "the stored item could not be read; unlock the keyring and retry";
    }
    return report;
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
    error.clear();
    const SecretToolResult result = run(commandFor("store", account, service), toHex(blob) + "\n");
    if (!result.error.empty() || result.status != 0 || stderrReportsFailure(result.diagnostics)) {
        error = result.error.empty()
            ? "secret-tool would not store the profile. Unlock the keyring and check the desktop D-Bus session."
            : result.error;
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
    error.clear();
    const SecretSearchReport report = search(account, service);
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
    error.clear();
    const SecretToolResult result = run(commandFor("clear", account, service));
    if (!result.error.empty() || result.status != 0 || stderrReportsFailure(result.diagnostics)) {
        error = result.error.empty()
            ? "The keyring would not forget the profile. Unlock the keyring and retry."
            : result.error;
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
    const SecretSearchReport outcome = search(account, service);
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
        unavailableReason.clear();
        const std::string probeAccount = "probe-" + std::to_string(::getpid()) + "-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        // Present is not the same as usable: secret-tool exits non-zero with no
        // message when the keyring is locked or no keyring daemon is running,
        // which is the state a freshly rebooted machine is in.
        std::string error;
        if (!sealViaTool(probeAccount, "com.kestrel.probe", {'p', 'r', 'o', 'b', 'e'}, error)) {
            unavailableReason = "the keyring did not accept a test value: " + error;
            return false;
        }
        const auto readBack = readViaTool(probeAccount, "com.kestrel.probe", error);
        const bool readable = readBack.has_value()
            && *readBack == std::vector<std::uint8_t>({'p', 'r', 'o', 'b', 'e'});
        // The probe has to come back out again, and a keyring that will accept
        // a write but refuse a delete is not a keyring this store can use: the
        // probe would sit there for the life of the process, and "forget
        // everything" would leave it behind.
        std::string cleanupError;
        if (!forgetViaTool(probeAccount, "com.kestrel.probe", cleanupError)) {
            unavailableReason =
                "the keyring accepted the test value but would not remove it: " + cleanupError;
            return false;
        }
        if (!readable) {
            unavailableReason = "The keyring accepted a probe but could not return its exact value. Unlock the keyring and retry.";
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
