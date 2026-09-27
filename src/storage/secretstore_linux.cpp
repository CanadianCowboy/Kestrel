#include "storage/secretstore.h"

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

// secret-tool prints the secret to stdout, so the output has to be collected
// separately from the input.
bool runCapturing(const std::string& command, std::string& output, std::string& error) {
    std::FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        error = "could not run " + command;
        return false;
    }
    std::array<char, 512> chunk{};
    output.clear();
    while (std::fgets(chunk.data(), static_cast<int>(chunk.size()), pipe) != nullptr) {
        output.append(chunk.data());
    }
    const int status = ::pclose(pipe);
    if (status != 0) {
        error = "the keyring command failed: " + command;
        return false;
    }
    return true;
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
    const std::string command = "secret-tool store --label='Kestrel profile' service " +
                                shellQuote(std::string(service)) + " account " +
                                shellQuote(std::string(account));
    std::FILE* pipe = ::popen(command.c_str(), "w");
    if (pipe == nullptr) {
        error = "could not run secret-tool";
        return false;
    }
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
    const std::string command = "secret-tool lookup service " + shellQuote(std::string(service)) +
                                " account " + shellQuote(std::string(account));
    std::string output;
    if (!runCapturing(command, output, error)) {
        return std::nullopt;
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    if (output.empty()) {
        // Nothing stored is a first run, not a failure.
        return std::nullopt;
    }
    auto bytes = fromHex(output);
    if (!bytes.has_value()) {
        error = "the keyring returned something that is not a sealed profile";
        return std::nullopt;
    }
    return bytes;
}

void forgetViaTool(std::string_view account, std::string_view service) {
    const std::string command = "secret-tool clear service " + shellQuote(std::string(service)) +
                                " account " + shellQuote(std::string(account));
    std::string ignored;
    // secret-tool exits non-zero when there was nothing to clear, which is the
    // state the caller asked for, so the result is not inspected.
    runCapturing(command, ignored, ignored);
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
        forgetViaTool("probe", "com.kestrel.probe");
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

    bool forget(std::string_view account, std::string_view service, std::string&) override {
        forgetViaTool(account, service);
        return true;
    }
};

} // namespace

std::unique_ptr<SecretStore> makePlatformSecretStore() {
    return std::make_unique<LinuxSecretStore>();
}

} // namespace kestrel::storage
