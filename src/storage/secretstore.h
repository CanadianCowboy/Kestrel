#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kestrel::storage {

// The platform's encrypted secret store, and the only place Kestrel's private
// data is ever allowed to be at rest.
//
// The store holds the sealed profile itself rather than a key that Kestrel then
// encrypts with. That is the whole reason this interface exists: sealing is
// then the operating system's job, done with a key derived from the user's own
// login, and this project never has to implement a cipher. A dependency-free
// AES would have been the alternative, and it would have been the weakest
// thing in the repository.
//
// Every implementation must fail loudly. A provider that cannot reach the
// platform keystore reports unavailable() with a reason, because the
// alternative -- quietly falling back to an unencrypted file -- is a silent
// downgrade of the only promise this store makes.
class SecretStore {
public:
    virtual ~SecretStore() = default;

    // A short description of where the bytes actually go, for the diagnostics
    // table: "Windows DPAPI (user scope)", "macOS keychain", and so on.
    [[nodiscard]] virtual std::string description() const = 0;

    // False when the platform keystore is not usable. `unavailableReason` is
    // then a sentence fit to show a user. There is no third state: either the
    // data is sealed by the platform, or Kestrel does not persist it.
    [[nodiscard]] virtual bool available(std::string& unavailableReason) const = 0;

    // Replaces the sealed blob stored under `account`, or reports why not.
    // Overwriting is the normal case, so implementations must not fail when the
    // account does not exist yet.
    [[nodiscard]] virtual bool seal(std::string_view account, std::string_view service,
                                    const std::vector<std::uint8_t>& blob, std::string& error) = 0;

    // Returns the sealed blob, or nullopt when there is nothing stored. A
    // missing account is not an error; an unreadable one is.
    [[nodiscard]] virtual std::optional<std::vector<std::uint8_t>> sealed(
        std::string_view account, std::string_view service, std::string& error) = 0;

    // Removes the stored blob. Removing something absent succeeds, so a user
    // who asks to forget everything twice is not told they cannot.
    [[nodiscard]] virtual bool forget(std::string_view account, std::string_view service,
                                      std::string& error) = 0;
};

// The service name Kestrel stores its profile under. Namespaced so a future
// second secret cannot collide with this one.
inline constexpr std::string_view kProfileService = "com.kestrel.profile";

// An in-memory implementation, used by the tests and by `--print-runtime` on a
// machine with no keystore. It is deliberately named for what it is: it seals
// nothing, so nothing may persist through it.
class InMemorySecretStore final : public SecretStore {
public:
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] bool available(std::string& unavailableReason) const override;
    [[nodiscard]] bool seal(std::string_view account, std::string_view service,
                            const std::vector<std::uint8_t>& blob, std::string& error) override;
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> sealed(
        std::string_view account, std::string_view service, std::string& error) override;
    [[nodiscard]] bool forget(std::string_view account, std::string_view service,
                              std::string& error) override;

private:
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> m_entries;
};

// The platform's real store. Defined once per platform in
// secretstore_windows.cpp and secretstore_posix.cpp.
[[nodiscard]] std::unique_ptr<SecretStore> makePlatformSecretStore();

} // namespace kestrel::storage
