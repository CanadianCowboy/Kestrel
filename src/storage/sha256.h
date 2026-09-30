#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace kestrel::storage {

// SHA-256, in the standard library only.
//
// This exists so a stored profile can be checked before it is trusted, and so
// the keystore's payload can be bound to the file it came from. It is a
// checksum against accident and casual tampering, not a defense against an
// attacker who can write the file: the confidentiality and the authenticity
// both come from the platform keystore, and this is the cheap tripwire that
// says "these bytes are not the bytes that were written".
//
// Written out rather than pulled in because a profile file is the one place
// where adding a dependency to a security boundary is a decision that should
// be made on purpose.
class Sha256 {
public:
    static constexpr std::size_t kDigestBytes = 32;

    Sha256() noexcept;

    void update(std::string_view data) noexcept;
    void update(const std::uint8_t* data, std::size_t size) noexcept;

    // Finalises and returns the digest. Further updates are undefined, so the
    // object is spent by this call.
    [[nodiscard]] std::array<std::uint8_t, kDigestBytes> finish() noexcept;

    // One-shot convenience for the common case.
    [[nodiscard]] static std::array<std::uint8_t, kDigestBytes> hash(std::string_view data) noexcept;

private:
    void compress(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> m_state{};
    std::array<std::uint8_t, 64> m_buffer{};
    std::size_t m_buffered = 0;
    std::uint64_t m_length = 0;
};

// Lowercase hex, the form used in the on-disk envelope and in test output.
[[nodiscard]] std::string toHex(const std::array<std::uint8_t, Sha256::kDigestBytes>& digest);

} // namespace kestrel::storage
