// What Spotify's protocol needs of cryptography, on mbedTLS (Wii U, Android) or OpenSSL (desktop).
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>

namespace spotify::crypto {

using Parts = std::initializer_list<std::pair<const uint8_t*, size_t>>;

// Fills `out` from the system's secure random source. False when there was none.
bool random(uint8_t* out, size_t n);
// out = base^exp mod m, written big-endian over all `m_len` bytes (zeros in front). The modulus is odd.
bool mod_exp(const uint8_t* base, size_t base_len, const uint8_t* exp, size_t exp_len, const uint8_t* m, size_t m_len,
             uint8_t* out);
void sha1(Parts parts, uint8_t out[20]);
void hmac_sha1(const uint8_t* key, size_t key_len, Parts parts, uint8_t out[20]);

// AES-128 encryption of single blocks, for counter mode.
class Aes128 {
public:
    explicit Aes128(const uint8_t key[16]);
    ~Aes128();
    Aes128(const Aes128&) = delete;
    Aes128& operator=(const Aes128&) = delete;
    void encrypt_block(const uint8_t in[16], uint8_t out[16]);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Spotify's audio files: AES-128 in counter mode from a fixed IV. XORs `data` (which starts `offset`
// bytes into the file's encrypted part) with the key stream, in place.
void audio_decrypt(Aes128& aes, uint64_t offset, uint8_t* data, size_t len);

}  // namespace spotify::crypto
