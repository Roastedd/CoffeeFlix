// Adapted from spotify-wiiu (https://github.com/Happynico7504/spotify-wiiu), MIT License,
// Copyright (c) 2026 Nico Christmann: see content/licenses/NOTICES.txt.
#include "services/spotify/spotify_shannon.hpp"
#include <cstring>

namespace spotify {

static inline uint32_t le32(const uint8_t *b) {
    return (uint32_t)b[0]        | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// ── Non-linear substitution functions (no table — pure rotation + XOR) ────────

uint32_t Shannon::sbox1(uint32_t w) {
    w ^= rotl(w, 5) | rotl(w, 7);
    w ^= rotl(w, 19) | rotl(w, 22);
    return w;
}

uint32_t Shannon::sbox2(uint32_t w) {
    w ^= rotl(w, 7) | rotl(w, 22);
    w ^= rotl(w, 5) | rotl(w, 19);
    return w;
}

// ── LFSR step ─────────────────────────────────────────────────────────────────

void Shannon::cycle() {
    uint32_t t = R[12] ^ R[13] ^ konst;
    t = sbox1(t) ^ rotl(R[0], 1);
    for (int i = 1; i < N; i++) R[i - 1] = R[i];
    R[N - 1] = t;
    t = sbox2(R[2] ^ R[N - 1]);
    R[0] ^= t;
    sbuf = t ^ R[8] ^ R[12];
}

void Shannon::diffuse() {
    for (int i = 0; i < N; i++) cycle();
}

// ── MAC accumulation ──────────────────────────────────────────────────────────

void Shannon::crcfunc(uint32_t i) {
    uint32_t t = CRC[0] ^ CRC[2] ^ CRC[N - 1] ^ i;
    for (int j = 1; j < N; j++) CRC[j - 1] = CRC[j];
    CRC[N - 1] = t;
}

void Shannon::macfunc(uint32_t i) {
    crcfunc(i);
    R[KEYP] ^= i;
}

// ── Key / nonce loading ───────────────────────────────────────────────────────

void Shannon::loadkey(const uint8_t *key, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t buf[4] = {0, 0, 0, 0};
        size_t chunk = (len - i < 4) ? (len - i) : 4;
        memcpy(buf, key + i, chunk);
        R[KEYP] ^= le32(buf);
        cycle();
        i += 4;
    }
    R[KEYP] ^= (uint32_t)len;
    cycle();

    // Save R into CRC before diffuse, then XOR diffused R back with CRC.
    // Matches the Rust shannon crate: self.CRC = self.R (before diffuse).
    memcpy(CRC, R, sizeof(R));
    diffuse();
    for (int j = 0; j < N; j++) R[j] ^= CRC[j];
}

void Shannon::genkonst() {
    konst = R[0];
}

void Shannon::reloadstate() {
    memcpy(R, initR, sizeof(R));
}

// ── Construction ──────────────────────────────────────────────────────────────
// Register initialised to Fibonacci sequence; CRC zeroed.

Shannon::Shannon(const uint8_t *key, size_t key_len) {
    memset(CRC, 0, sizeof(CRC));
    R[0] = 1;
    R[1] = 1;
    for (int i = 2; i < N; i++) R[i] = R[i - 1] + R[i - 2];
    konst = INITKONST;
    sbuf = 0; mbuf = 0; nbuf = 0;
    loadkey(key, key_len);
    genkonst();
    memcpy(initR, R, sizeof(R));
}

// ── Per-packet nonce ──────────────────────────────────────────────────────────
// Spotify sends the packet sequence number big-endian.

void Shannon::nonce(uint32_t n) {
    uint8_t buf[4] = {
        (uint8_t)(n >> 24), (uint8_t)(n >> 16),
        (uint8_t)(n >> 8),  (uint8_t)(n)
    };
    reloadstate();
    konst = INITKONST;
    loadkey(buf, 4);
    genkonst();
    sbuf = 0; nbuf = 0;
}

// ── Encrypt ───────────────────────────────────────────────────────────────────
// Accumulate plaintext into MAC, then XOR with keystream.

void Shannon::encrypt(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (nbuf == 0) { cycle(); nbuf = 32; }
        mbuf ^= (uint32_t)buf[i] << (32 - nbuf);
        buf[i] ^= (uint8_t)(sbuf >> (32 - nbuf));
        nbuf -= 8;
        if (nbuf == 0) { macfunc(mbuf); mbuf = 0; }
    }
}

// ── Decrypt ───────────────────────────────────────────────────────────────────
// XOR with keystream, then accumulate plaintext into MAC.

void Shannon::decrypt(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (nbuf == 0) { cycle(); nbuf = 32; }
        buf[i] ^= (uint8_t)(sbuf >> (32 - nbuf));
        mbuf ^= (uint32_t)buf[i] << (32 - nbuf);
        nbuf -= 8;
        if (nbuf == 0) { macfunc(mbuf); mbuf = 0; }
    }
}

// ── Finish ────────────────────────────────────────────────────────────────────
// Flush any partial MAC word, fold CRC into R, diffuse, emit MAC bytes.

void Shannon::finish(uint8_t mac[MAC_SIZE]) {
    if (nbuf != 0) macfunc(mbuf);
    cycle();
    R[KEYP] ^= INITKONST ^ ((uint32_t)nbuf << 3);
    nbuf = 0; mbuf = 0;

    for (int i = 0; i < N; i++) R[i] ^= CRC[i];
    diffuse();

    int out_nbuf = 0;
    for (size_t i = 0; i < MAC_SIZE; i++) {
        if (out_nbuf == 0) { cycle(); out_nbuf = 32; }
        mac[i] = (uint8_t)(sbuf >> (32 - out_nbuf));
        out_nbuf -= 8;
    }
}

} // namespace spotify
