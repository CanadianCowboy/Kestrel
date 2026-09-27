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

// The keychain is searched by an opaque label rather than by the account name.
// A label is something a user can read in Keychain Access, and putting a
// conversation-derived name in it would leak the name into a field every tool
// on the machine can enumerate.
CFStringRef labelFor(std::string_view service, std::string_view account) {
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

void put(CFMutableDictionaryRef dictionary, CFStringRef key, CFTypeRef value) {
    CFDictionarySetValue(dictionary, key, value);
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
    CFStringRef label = labelFor(service, account);
    CFStringRef accessible = CFStringCreateWithCString(kCFAllocatorDefault,
                                                       "kSecAttrAccessibleAfterFirstUnlock",
                                                       kCFStringEncodingUTF8);
    // The default accessibility class would show a prompt every time the
    // assistant starts. This one is readable once the user has logged in,
    // which is the only time Kestrel runs.
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, blob.data(),
                                  static_cast<CFIndex>(blob.size()));
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                             &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    put(query, kSecClass, kSecClassGenericPassword);
    put(query, kSecAttrService, serviceRef);
    put(query, kSecAttrLabel, label);
    put(query, kSecAttrAccessible, accessible);
    put(query, kSecValueData, data);

    // Update-or-add. Adding alone fails with errSecDuplicateItem on every save
    // after the first, which would mean a profile that can only ever be written
    // once.
    //
    // Apple's documentation for SecItemUpdate specifies NULL for
    // attributesToUpdate to mean "every attribute named in the query", which is
    // exactly what a replace needs, and the SDK header marks that parameter
    // nonnull regardless. The warning is about the annotation, not the call.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnonnull"
    const OSStatus updated = SecItemUpdate(query, nullptr);
    OSStatus status = updated;
    if (updated == errSecItemNotFound) {
        status = SecItemAdd(query, nullptr);
    }
#pragma clang diagnostic pop

    CFRelease(query);
    CFRelease(data);
    CFRelease(accessible);
    CFRelease(label);
    CFRelease(serviceRef);

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
    CFStringRef label = labelFor(service, account);
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                             &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    put(query, kSecClass, kSecClassGenericPassword);
    put(query, kSecAttrService, serviceRef);
    put(query, kSecAttrLabel, label);
    put(query, kSecReturnData, kCFBooleanTrue);
    put(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef found = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &found);

    CFRelease(query);
    CFRelease(label);
    CFRelease(serviceRef);

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

void forgetFromKeychain(std::string_view account, std::string_view service, std::string& error) {
    CFStringRef serviceRef = copyString(service);
    CFStringRef label = labelFor(service, account);
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                             &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    put(query, kSecClass, kSecClassGenericPassword);
    put(query, kSecAttrService, serviceRef);
    put(query, kSecAttrLabel, label);
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    CFRelease(label);
    CFRelease(serviceRef);

    if (status == errSecSuccess || status == errSecItemNotFound) {
        // Deleting what was not there is the state the caller asked for.
        return;
    }
    error = "the keychain would not forget the profile: " + statusText(status);
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
        forgetFromKeychain("probe", "com.kestrel.probe", error);
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
        forgetFromKeychain(account, service, error);
        return error.empty();
    }
};

} // namespace

std::unique_ptr<SecretStore> makePlatformSecretStore() {
    return std::make_unique<MacSecretStore>();
}

} // namespace kestrel::storage
