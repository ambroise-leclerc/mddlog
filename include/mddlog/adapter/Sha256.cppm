/** @brief SHA-256 (FIPS 180-4), self-contained, for the audit chain of ADR-004 Decision 8.4. */

export module mddlog.adapter.sha256;

import std;

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers,hicpp-signed-bitwise,cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-pointer-arithmetic,readability-math-missing-parentheses):
// FIPS 180-4 constants, rotation counts and word/byte arithmetic are the specification itself; indices are bounded by the fixed block and round counts.
export namespace mddlog::adapter {

inline constexpr std::size_t sha256DigestSize = 32;

using Sha256Digest = std::array<std::uint8_t, sha256DigestSize>;

/**
 * @brief Incremental SHA-256 with no allocation, no I/O and no key.
 *
 * It lives in the adapter zone (ADR-004 Decision 2): the governed core hashes nothing. Usable in
 * constant expressions, so the reference vectors are also checked at compile time.
 */
class Sha256 {
public:
    constexpr Sha256() noexcept = default;

    constexpr void update(std::span<const std::uint8_t> data) noexcept {
        for (const std::uint8_t byte : data) {
            block[blockFill++] = byte;
            if (blockFill == blockSize) {
                compress();
                blockFill = 0;
            }
        }
        totalBytes += data.size();
    }

    /** @brief Pad and return the digest; the object must not be reused afterwards. */
    [[nodiscard]] constexpr Sha256Digest finish() noexcept {
        const std::uint64_t bitLength = totalBytes * 8;
        block[blockFill++]            = 0x80;
        if (blockFill > blockSize - 8) {
            while (blockFill < blockSize)
                block[blockFill++] = 0;
            compress();
            blockFill = 0;
        }
        while (blockFill < blockSize - 8)
            block[blockFill++] = 0;
        for (int shift = 56; shift >= 0; shift -= 8)
            block[blockFill++] = static_cast<std::uint8_t>(bitLength >> shift);
        compress();
        Sha256Digest out{};
        for (std::size_t word = 0; word < state.size(); ++word)
            for (std::size_t byte = 0; byte < 4; ++byte)
                out[word * 4 + byte] = static_cast<std::uint8_t>(state[word] >> (24 - 8 * byte));
        return out;
    }

private:
    static constexpr std::size_t blockSize = 64;

    static constexpr std::array<std::uint32_t, 64> roundConstants{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    [[nodiscard]] static constexpr std::uint32_t rotr(std::uint32_t value, int count) noexcept {
        return (value >> count) | (value << (32 - count));
    }

    constexpr void compress() noexcept {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i)
            w[i] = (std::uint32_t{block[i * 4]} << 24) | (std::uint32_t{block[i * 4 + 1]} << 16) | (std::uint32_t{block[i * 4 + 2]} << 8)
                   | std::uint32_t{block[i * 4 + 3]};
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i]                   = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, h] = state;
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1    = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch    = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + ch + roundConstants[i] + w[i];
            const std::uint32_t s0    = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            h                         = g;
            g                         = f;
            f                         = e;
            e                         = d + temp1;
            d                         = c;
            c                         = b;
            b                         = a;
            a                         = temp1 + temp2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    std::array<std::uint32_t, 8>        state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, blockSize> block{};
    std::size_t                         blockFill  = 0;
    std::uint64_t                       totalBytes = 0;
};

/** @brief One-shot SHA-256 of a byte range. */
[[nodiscard]] constexpr Sha256Digest sha256(std::span<const std::uint8_t> data) noexcept {
    Sha256 hasher;
    hasher.update(data);
    return hasher.finish();
}

}  // namespace mddlog::adapter
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers,hicpp-signed-bitwise,cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-pointer-arithmetic,readability-math-missing-parentheses)
