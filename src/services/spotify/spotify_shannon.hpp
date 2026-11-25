// Adapted from spotify-wiiu (https://github.com/Happynico7504/spotify-wiiu), MIT License,
// Copyright (c) 2026 Nico Christmann: see content/licenses/NOTICES.txt.
#pragma once
#include <cstdint>
#include <cstddef>

// Shannon stream cipher — Spotify AP packet encryption + MAC.
// Ported from https://github.com/plietar/rust-shannon (used by librespot).
//
// Usage per packet:
//   nonce(seq_number)         — reset + reseed with big-endian packet counter
//   encrypt(buf, len)         — XOR plaintext → ciphertext, accumulate MAC
//     OR decrypt(buf, len)   — XOR ciphertext → plaintext, accumulate MAC
//   finish(mac)               — produce 4-byte MAC

namespace spotify {

class Shannon {
public:
    static constexpr size_t MAC_SIZE = 4;

    explicit Shannon(const uint8_t *key, size_t key_len);

    void nonce(uint32_t n);                   // big-endian 4-byte nonce
    void encrypt(uint8_t *buf, size_t len);
    void decrypt(uint8_t *buf, size_t len);
    void finish(uint8_t mac[MAC_SIZE]);

private:
    static constexpr int      N         = 16;
    static constexpr uint32_t INITKONST = 0x6996c53au;
    static constexpr int      KEYP      = 13;

    uint32_t R[N];
    uint32_t CRC[N];
    uint32_t initR[N];
    uint32_t konst = INITKONST;
    uint32_t sbuf  = 0;
    uint32_t mbuf  = 0;
    int      nbuf  = 0;

    static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
    static uint32_t sbox1(uint32_t w);
    static uint32_t sbox2(uint32_t w);

    void cycle();
    void crcfunc(uint32_t i);
    void macfunc(uint32_t i);
    void loadkey(const uint8_t *key, size_t len);
    void genkonst();
    void reloadstate();
    void diffuse();
};

} // namespace spotify
