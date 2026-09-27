#include "storage/secretstore.h"

#include "storage/atomicfile.h"
#include "storage/paths.h"
#include "storage/sha256.h"

// The one translation unit permitted to include a Windows header for the
// storage layer, for the same reason cudadiscovery_cuda.cpp is the only one
// permitted to include a CUDA header: the platform API has to be contained so
// everything above it stays portable and testable.

#include <windows.h>
// crypt32 is the only import library this needs and it is only ever needed
// here, so it is linked where it is used rather than widening the target's
// dependencies for a file that is not compiled on the other platforms.
#pragma comment(lib, "crypt32.lib")

#include <filesystem>
#include <string>
#include <vector>

namespace kestrel::storage {
namespace {

// DPAPI entropy is mixed into the key it derives, so a blob sealed for one
// purpose cannot be unsealed under another. It is not a secret; it is a label
// that has to travel with the bytes.
constexpr char kEntropy[] = "Kestrel profile v1";

std::wstring widen(const char* text) {
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), length);
    if (!wide.empty() && wide.back() == L'\0') {
        wide.pop_back();
    }
    return wide;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string narrowText(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrowText.data(),
                          length, nullptr, nullptr);
    return narrowText;
}

std::string windowsError(DWORD code) {
    LPWSTR buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string message;
    if (length != 0 && buffer != nullptr) {
        message = narrow(std::wstring(buffer, length));
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    return message.empty() ? ("Windows error " + std::to_string(code)) : message;
}

// The blob's filename is derived, never taken from the account name, because
// an account string is something a caller chooses and a path is something the
// filesystem has opinions about.
std::filesystem::path blobPath(std::string_view service, std::string_view account) {
    const std::string composed = std::string(service) + "\n" + std::string(account);
    const std::string name = toHex(Sha256::hash(composed));
    return sealedBlobDirectory() / (name + ".sealed");
}

// Whether there is anywhere to put a blob at all.
//
// paths.h defines an empty directory as "do not persist", and an empty
// directory here would make blobPath resolve to a bare relative filename. The
// profile would then be written into whatever directory the process happened to
// be started in -- which on Windows is often the install directory, and is
// never where the user would look for it, or be able to delete it. Every entry
// point has to ask, not just available(): a caller that has already decided to
// save is exactly the caller that will not check first.
bool haveBlobDirectory(std::string& error) {
    if (sealedBlobDirectory().empty()) {
        error = "LOCALAPPDATA is not set, so there is nowhere to keep a sealed profile.";
        return false;
    }
    return true;
}

class WindowsSecretStore final : public SecretStore {
public:
    std::string description() const override {
        return "Windows DPAPI (user scope)";
    }

    bool available(std::string& unavailableReason) const override {
        const std::filesystem::path directory = sealedBlobDirectory();
        if (directory.empty()) {
            unavailableReason =
                "LOCALAPPDATA is not set, so there is nowhere to keep a sealed profile.";
            return false;
        }
        // DPAPI has no capability to probe, so the only honest test is to seal
        // something and throw it away. A machine where that fails would fail
        // for the real profile too, and finding out here turns silent data
        // loss into a message the user can read.
        const std::vector<std::uint8_t> probe{'p', 'r', 'o', 'b', 'e'};
        std::vector<std::uint8_t> sealedBytes;
        std::string error;
        if (!cryptProtect(probe, sealedBytes, error)) {
            unavailableReason = error;
            return false;
        }
        // And the sealed bytes have to actually reach the disk before the
        // removal below means anything. Deleting a file that was never written
        // succeeds -- "was never there" is the state the caller asked for --
        // so a probe that only sealed and then deleted was testing DPAPI and
        // nothing else. A directory that accepts no writes, or that is
        // read-only, passed it, and the first save of a real profile is where
        // that would have been found instead.
        if (!writeFileAtomically(blobPath("com.kestrel.probe", "probe"), sealedBytes, error)) {
            unavailableReason = "the keystore would not store a test value: " + error;
            return false;
        }
        // The probe has to come back out again. A failure here is not cosmetic:
        // it means the directory accepts a write and refuses a delete, which is
        // the state where "forget everything" would leave the probe behind.
        std::string cleanupError;
        if (!removeBlob("com.kestrel.probe", "probe", cleanupError)) {
            unavailableReason =
                "the keystore accepted the test value but would not remove it: " + cleanupError;
            return false;
        }
        return true;
    }

    bool seal(std::string_view account, std::string_view service,
              const std::vector<std::uint8_t>& blob, std::string& error) override {
        if (!haveBlobDirectory(error)) {
            return false;
        }
        std::vector<std::uint8_t> sealedBytes;
        if (!cryptProtect(blob, sealedBytes, error)) {
            return false;
        }
        // DPAPI seals but does not store. The sealed bytes are what a file
        // holds; the file is not readable by another account, another machine,
        // or another user, and on its own it is inert.
        return writeFileAtomically(blobPath(service, account), sealedBytes, error);
    }

    std::optional<std::vector<std::uint8_t>> sealed(std::string_view account,
                                                    std::string_view service,
                                                    std::string& error) override {
        if (!haveBlobDirectory(error)) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> sealedBytes;
        if (!readWholeFile(blobPath(service, account), sealedBytes, error)) {
            return std::nullopt;
        }
        if (sealedBytes.empty()) {
            // An absent profile is a first run, not a failure.
            return std::nullopt;
        }
        return cryptUnprotect(sealedBytes, error);
    }

    bool forget(std::string_view account, std::string_view service, std::string& error) override {
        if (!haveBlobDirectory(error)) {
            return false;
        }
        return removeBlob(service, account, error);
    }

private:
    // A removal that fails is a removal that did not happen, and the caller is
    // about to tell a user that Kestrel has forgotten them. The only status that
    // counts as success is one where the file is gone, or was never there.
    static bool removeBlob(std::string_view service, std::string_view account, std::string& error) {
        const std::filesystem::path path = blobPath(service, account);
        std::error_code code;
        const bool removed = std::filesystem::remove(path, code);
        if (removed) {
            return true;
        }
        if (std::error_code exists;
            !std::filesystem::exists(path, exists) && !exists) {
            // Never there is the state the caller asked for.
            return true;
        }
        error = "could not remove " + path.string() + ": " +
                (code ? code.message() : std::string("the file is still there"));
        return false;
    }

    static bool cryptProtect(const std::vector<std::uint8_t>& plain,
                             std::vector<std::uint8_t>& sealedOut, std::string& error) {
        DATA_BLOB input{};
        input.pbData = const_cast<BYTE*>(plain.data());
        input.cbData = static_cast<DWORD>(plain.size());

        DATA_BLOB entropy{};
        entropy.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
        entropy.cbData = static_cast<DWORD>(sizeof(kEntropy));

        DATA_BLOB output{};
        if (!::CryptProtectData(&input, L"Kestrel profile", &entropy, nullptr, nullptr,
                                CRYPTPROTECT_UI_FORBIDDEN, &output)) {
            error = "DPAPI refused to seal the profile: " + windowsError(::GetLastError());
            return false;
        }
        sealedOut.assign(output.pbData, output.pbData + output.cbData);
        ::SecureZeroMemory(output.pbData, output.cbData);
        ::LocalFree(output.pbData);
        return true;
    }

    static std::optional<std::vector<std::uint8_t>> cryptUnprotect(
        const std::vector<std::uint8_t>& sealedBytes, std::string& error) {
        DATA_BLOB input{};
        input.pbData = const_cast<BYTE*>(sealedBytes.data());
        input.cbData = static_cast<DWORD>(sealedBytes.size());

        DATA_BLOB entropy{};
        entropy.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
        entropy.cbData = static_cast<DWORD>(sizeof(kEntropy));

        DATA_BLOB output{};
        if (!::CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                                  CRYPTPROTECT_UI_FORBIDDEN, &output)) {
            // A blob this account cannot open is indistinguishable from a
            // corrupt one as far as DPAPI is concerned, and both mean the same
            // thing to a user: the profile is not recoverable here. Saying which
            // would be a guess.
            error = "this profile was sealed for a different Windows account or is damaged: " +
                    windowsError(::GetLastError());
            return std::nullopt;
        }
        std::vector<std::uint8_t> plain(output.pbData, output.pbData + output.cbData);
        ::SecureZeroMemory(output.pbData, output.cbData);
        ::LocalFree(output.pbData);
        return plain;
    }
};

} // namespace

std::unique_ptr<SecretStore> makePlatformSecretStore() {
    return std::make_unique<WindowsSecretStore>();
}

} // namespace kestrel::storage
