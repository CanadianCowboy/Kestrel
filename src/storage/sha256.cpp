#include "storage/sha256.h"

#include <algorithm>
#include <cstring>

namespace kestrel::storage {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned bits) noexcept {
    return (value >> bits) | (value << (32u - bits));
}

} // namespace

Sha256::Sha256() noexcept
    : m_state{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u} {}

void Sha256::compress(const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = m_state[0];
    std::uint32_t b = m_state[1];
    std::uint32_t c = m_state[2];
    std::uint32_t d = m_state[3];
    std::uint32_t e = m_state[4];
    std::uint32_t f = m_state[5];
    std::uint32_t g = m_state[6];
    std::uint32_t h = m_state[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + choose + kRoundConstants[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t size) noexcept {
    m_length += static_cast<std::uint64_t>(size) * 8u;
    std::size_t offset = 0;
    while (offset < size) {
        const std::size_t take = std::min(size - offset, m_buffer.size() - m_buffered);
        std::memcpy(m_buffer.data() + m_buffered, data + offset, take);
        m_buffered += take;
        offset += take;
        if (m_buffered == m_buffer.size()) {
            compress(m_buffer.data());
            m_buffered = 0;
        }
    }
}

void Sha256::update(std::string_view data) noexcept {
    update(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

std::array<std::uint8_t, Sha256::kDigestBytes> Sha256::finish() noexcept {
    const std::uint64_t length = m_length;
    const std::uint8_t one = 0x80;
    update(&one, 1);
    const std::uint8_t zero = 0x00;
    while (m_buffered != 56) {
        update(&zero, 1);
    }

    // The length is appended big-endian. update() would add to the counter it
    // is about to be read from, so it is written straight into the buffer.
    std::array<std::uint8_t, 8> tail{};
    for (std::size_t i = 0; i < 8; ++i) {
        tail[i] = static_cast<std::uint8_t>((length >> (56u - 8u * i)) & 0xffu);
    }
    std::memcpy(m_buffer.data() + 56, tail.data(), tail.size());
    compress(m_buffer.data());
    m_buffered = 0;

    std::array<std::uint8_t, kDigestBytes> digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>((m_state[i] >> 24) & 0xffu);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((m_state[i] >> 16) & 0xffu);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((m_state[i] >> 8) & 0xffu);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(m_state[i] & 0xffu);
    }
    return digest;
}

std::array<std::uint8_t, Sha256::kDigestBytes> Sha256::hash(std::string_view data) noexcept {
    Sha256 sha;
    sha.update(data);
    return sha.finish();
}

std::string toHex(const std::array<std::uint8_t, Sha256::kDigestBytes>& digest) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        text.push_back(kDigits[(byte >> 4) & 0x0f]);
        text.push_back(kDigits[byte & 0x0f]);
    }
    return text;
}

} // namespace kestrel::storage
