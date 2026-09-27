#include "storage/secretstore.h"

#include "storage/sha256.h"

// The one translation unit permitted to include a macOS-only header for the
// storage layer, for the same reason the CUDA and Windows ones are singular:
// the platform API stays contained so everything above it is portable.
//
// Written against CoreFoundation rather than Foundation so this stays a .cpp.
// The Objective-C spellings of the same calls would drag an Objective-C++ file
// and a language mode into a library that is otherwise plain C++20, and the
// only thing gained is brevity in a file that exists precisely because it is
// allowed to be platform-shaped.

#include <Security/Security.h>

#include <string>
#include <vector>

namespace kestrel::storage {
namespace {

std::string statusText(OSStatus status) {
    // Copy before releasing: the buffer belongs to the framework, and the
    // message has to outlive the call that produced it.
    CFStringRef message = SecCopyErrorMessageString(status, nullptr);
    std::string text = "OSStatus " + std::to_string(static_cast<long>(status));
    if (message != nullptr) {
        const char* utf8 = CFStringGetCStringPtr(message, kCFStringEncodingUTF8);
        if (utf8 != nullptr) {
            text = utf8;
        }
        CFRelease(message);
    }
    return text;
}

CFStringRef copyString(std::string_view text) {
    return CFStringCreateWithBytes(kCFAllocatorDefault,
                                   reinterpret_cast<const UInt8*>(text.data()),
                                   static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false);
}

// A generic-password item is identified by exactly three things: its class, its
// service and its account. Nothing else takes part, so the account is where the
// per-profile separation has to live.
//
// It cannot be the account name itself. Keychain Access is a window every tool
// on the machine can open, and anything in kSecAttrAccount is a line of text in
// it, so a conversation-derived name would sit there in plain sight. The digest
// separates just as well and reads as nothing.
CFStringRef accountKeyFor(std::string_view service, std::string_view account) {
    std::string composed(service);
    composed.push_back('\n');
    composed.append(account);
    const auto digest = Sha256::hash(composed);
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        hex.push_back(kDigits[(byte >> 4) & 0x0f]);
        hex.push_back(kDigits[byte & 0x0f]);
    }
    return copyString(hex);
}

CFMutableDictionaryRef makeDictionary() {
    return CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                     &kCFTypeDictionaryValueCallBacks);
}

void put(CFMutableDictionaryRef dictionary, CFStringRef key, CFTypeRef value) {
    CFDictionarySetValue(dictionary, key, value);
}

// The search keys, and only the search keys. A keychain query that also carries
// the new values is not the same thing as one that does not: the values are
// what a replace changes, and mixing them into the query is how an update ends
// up matching nothing.
void putSearchKeys(CFMutableDictionaryRef dictionary, CFStringRef service, CFStringRef account) {
    put(dictionary, kSecClass, kSecClassGenericPassword);
    put(dictionary, kSecAttrService, service);
    put(dictionary, kSecAttrAccount, account);
}

// The three operations below are free functions rather than members for a
// reason that is not stylistic: available() is const, and it has to perform a
// real write and a real delete to find out whether a locked keychain will
// accept anything. A const member cannot call a non-const member, so a probe
// written in terms of seal() and forget() does not compile. This is the same
// shape the Windows backend uses for its probe.
bool sealInKeychain(std::string_view account, std::string_view service,
                    const std::vector<std::uint8_t>& blob, std::string& error) {
    CFStringRef serviceRef = copyString(service);
    CFStringRef accountRef = accountKeyFor(service, account);
    CFStringRef label = copyString("Kestrel profile");
    // The framework's constant, not a string spelling of its name. The value of
    // kSecAttrAccessibleAfterFirstUnlock is a short opaque tag; the identifier
    // text would be rejected as an accessibility class or ignored. The default
    // class would also prompt every time the assistant starts, and this one is
    // readable once the user has logged in, which is the only time Kestrel runs.
    CFStringRef accessible = kSecAttrAccessibleAfterFirstUnlock;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, blob.data(),
                                  static_cast<CFIndex>(blob.size()));

    CFMutableDictionaryRef search = makeDictionary();
    putSearchKeys(search, serviceRef, accountRef);

    CFMutableDictionaryRef attributes = makeDictionary();
    put(attributes, kSecAttrLabel, label);
    put(attributes, kSecAttrAccessible, accessible);
    put(attributes, kSecValueData, data);

    // Update-or-add, and the add branch has to be reachable: an item that is
    // only ever added fails with errSecDuplicateItem on the second save, which
    // would mean a profile that can be written once and never again.
    OSStatus status = SecItemUpdate(search, attributes);
    if (status == errSecItemNotFound) {
        // An add needs the search keys and the new values in one dictionary,
        // because that is the only shape SecItemAdd accepts.
        CFMutableDictionaryRef fresh = makeDictionary();
        putSearchKeys(fresh, serviceRef, accountRef);
        put(fresh, kSecAttrLabel, label);
        put(fresh, kSecAttrAccessible, accessible);
        put(fresh, kSecValueData, data);
        status = SecItemAdd(fresh, nullptr);
        CFRelease(fresh);
    }

    CFRelease(search);
    CFRelease(attributes);
    // data, label, serviceRef and accountRef are ours to release. `accessible`
    // is a framework constant, and releasing it would be a bug of its own.
    CFRelease(data);
    CFRelease(label);
    CFRelease(serviceRef);
    CFRelease(accountRef);

    if (status != errSecSuccess) {
        error = "the keychain would not store the profile: " + statusText(status);
        return false;
    }
    return true;
}

std::optional<std::vector<std::uint8_t>> readFromKeychain(std::string_view account,
                                                          std::string_view service,
                                                          std::string& error) {
    CFStringRef serviceRef = copyString(service);
    CFStringRef accountRef = accountKeyFor(service, account);
    CFMutableDictionaryRef query = makeDictionary();
    putSearchKeys(query, serviceRef, accountRef);
    put(query, kSecReturnData, kCFBooleanTrue);
    put(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef found = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &found);

    CFRelease(query);
    CFRelease(serviceRef);
    CFRelease(accountRef);

    if (status == errSecItemNotFound) {
        // Nothing stored is a first run, not a failure.
        return std::nullopt;
    }
    if (status != errSecSuccess) {
        error = "the keychain would not release the profile: " + statusText(status);
        return std::nullopt;
    }
    const CFDataRef data = static_cast<CFDataRef>(found);
    const CFIndex length = CFDataGetLength(data);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    if (length > 0) {
        CFDataGetBytes(data, CFRangeMake(0, length), bytes.data());
    }
    CFRelease(found);
    return bytes;
}

bool forgetFromKeychain(std::string_view account, std::string_view service, std::string& error) {
    CFStringRef serviceRef = copyString(service);
    CFStringRef accountRef = accountKeyFor(service, account);
    CFMutableDictionaryRef query = makeDictionary();
    putSearchKeys(query, serviceRef, accountRef);
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    CFRelease(serviceRef);
    CFRelease(accountRef);

    if (status == errSecSuccess || status == errSecItemNotFound) {
        // Deleting what was not there is the state the caller asked for.
        return true;
    }
    // Anything else means the profile is still in the keychain, and saying
    // otherwise is the one thing this store must not do.
    error = "the keychain would not forget the profile: " + statusText(status);
    return false;
}

class MacSecretStore final : public SecretStore {
public:
    std::string description() const override {
        return "macOS keychain (login keychain, user scope)";
    }

    bool available(std::string& unavailableReason) const override {
        // The login keychain is always addressable; the failure mode is that it
        // is locked, and only a real write can reveal that.
        std::string error;
        if (!sealInKeychain("probe", "com.kestrel.probe", {'p', 'r', 'o', 'b', 'e'}, error)) {
            unavailableReason = "the keychain did not accept a test value: " + error;
            return false;
        }
        // The probe has to come back out again, and that is a second thing that
        // can fail on its own. A keychain that will store a value but not give
        // it back is not a keychain this store can use, and leaving the probe
        // behind would mean reporting itself available while sitting on a
        // value that would not go away -- the one claim this store must not
        // make.
        std::string cleanupError;
        if (!forgetFromKeychain("probe", "com.kestrel.probe", cleanupError)) {
            unavailableReason =
                "the keychain accepted a test value but would not remove it: " + cleanupError;
            return false;
        }
        return true;
    }

    bool seal(std::string_view account, std::string_view service,
              const std::vector<std::uint8_t>& blob, std::string& error) override {
        return sealInKeychain(account, service, blob, error);
    }

    std::optional<std::vector<std::uint8_t>> sealed(std::string_view account,
                                                    std::string_view service,
                                                    std::string& error) override {
        return readFromKeychain(account, service, error);
    }

    bool forget(std::string_view account, std::string_view service, std::string& error) override {
        return forgetFromKeychain(account, service, error);
    }
};

} // namespace

std::unique_ptr<SecretStore> makePlatformSecretStore() {
    return std::make_unique<MacSecretStore>();
}

} // namespace kestrel::storage
