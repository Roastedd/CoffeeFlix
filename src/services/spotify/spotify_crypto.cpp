#include "services/spotify/spotify_crypto.hpp"

#include <cstring>

#if defined(__WIIU__) || defined(__ANDROID__)
#define SPOTIFY_MBEDTLS 1
#include <mbedtls/aes.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/md.h>
#else
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#endif

namespace spotify::crypto {

#ifdef SPOTIFY_MBEDTLS

bool random(uint8_t* out, size_t n) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    static const uint8_t PERS[] = "coffeeflix-spotify";
    bool ok = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, PERS, sizeof(PERS) - 1) == 0 &&
              mbedtls_ctr_drbg_random(&drbg, out, n) == 0;
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return ok;
}

bool mod_exp(const uint8_t* base, size_t base_len, const uint8_t* exp, size_t exp_len, const uint8_t* m, size_t m_len,
             uint8_t* out) {
    mbedtls_mpi B, E, M, R;
    mbedtls_mpi_init(&B), mbedtls_mpi_init(&E), mbedtls_mpi_init(&M), mbedtls_mpi_init(&R);
    bool ok = mbedtls_mpi_read_binary(&B, base, base_len) == 0 && mbedtls_mpi_read_binary(&E, exp, exp_len) == 0 &&
              mbedtls_mpi_read_binary(&M, m, m_len) == 0 && mbedtls_mpi_exp_mod(&R, &B, &E, &M, nullptr) == 0 &&
              mbedtls_mpi_write_binary(&R, out, m_len) == 0;
    mbedtls_mpi_free(&B), mbedtls_mpi_free(&E), mbedtls_mpi_free(&M), mbedtls_mpi_free(&R);
    return ok;
}

namespace {
void md_sha1(const uint8_t* key, size_t key_len, bool hmac, Parts parts, uint8_t out[20]) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), hmac ? 1 : 0);
    if (hmac) {
        mbedtls_md_hmac_starts(&ctx, key, key_len);
        for (auto& [p, n] : parts) mbedtls_md_hmac_update(&ctx, p, n);
        mbedtls_md_hmac_finish(&ctx, out);
    } else {
        mbedtls_md_starts(&ctx);
        for (auto& [p, n] : parts) mbedtls_md_update(&ctx, p, n);
        mbedtls_md_finish(&ctx, out);
    }
    mbedtls_md_free(&ctx);
}
}  // namespace

void sha1(Parts parts, uint8_t out[20]) { md_sha1(nullptr, 0, false, parts, out); }
void hmac_sha1(const uint8_t* key, size_t key_len, Parts parts, uint8_t out[20]) { md_sha1(key, key_len, true, parts, out); }

struct Aes128::Impl {
    mbedtls_aes_context ctx;
};
Aes128::Aes128(const uint8_t key[16]) : impl_(new Impl) {
    mbedtls_aes_init(&impl_->ctx);
    mbedtls_aes_setkey_enc(&impl_->ctx, key, 128);
}
Aes128::~Aes128() { mbedtls_aes_free(&impl_->ctx); }
void Aes128::encrypt_block(const uint8_t in[16], uint8_t out[16]) {
    mbedtls_aes_crypt_ecb(&impl_->ctx, MBEDTLS_AES_ENCRYPT, in, out);
}

#else  // OpenSSL

bool random(uint8_t* out, size_t n) { return RAND_bytes(out, (int)n) == 1; }

bool mod_exp(const uint8_t* base, size_t base_len, const uint8_t* exp, size_t exp_len, const uint8_t* m, size_t m_len,
             uint8_t* out) {
    BN_CTX* ctx = BN_CTX_new();
    BIGNUM *B = BN_bin2bn(base, (int)base_len, nullptr), *E = BN_bin2bn(exp, (int)exp_len, nullptr),
           *M = BN_bin2bn(m, (int)m_len, nullptr), *R = BN_new();
    bool ok = ctx && B && E && M && R && BN_mod_exp(R, B, E, M, ctx) == 1 && BN_bn2binpad(R, out, (int)m_len) == (int)m_len;
    BN_free(B), BN_free(E), BN_free(M), BN_free(R);
    BN_CTX_free(ctx);
    return ok;
}

void sha1(Parts parts, uint8_t out[20]) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    for (auto& [p, n] : parts) EVP_DigestUpdate(ctx, p, n);
    unsigned len = 20;
    EVP_DigestFinal_ex(ctx, out, &len);
    EVP_MD_CTX_free(ctx);
}

void hmac_sha1(const uint8_t* key, size_t key_len, Parts parts, uint8_t out[20]) {
    EVP_MAC* mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(mac);
    char digest[] = "SHA1";
    OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string("digest", digest, 0), OSSL_PARAM_construct_end()};
    EVP_MAC_init(ctx, key, key_len, params);
    for (auto& [p, n] : parts) EVP_MAC_update(ctx, p, n);
    size_t len = 0;
    EVP_MAC_final(ctx, out, &len, 20);
    EVP_MAC_CTX_free(ctx);
    EVP_MAC_free(mac);
}

struct Aes128::Impl {
    EVP_CIPHER_CTX* ctx = nullptr;
};
Aes128::Aes128(const uint8_t key[16]) : impl_(new Impl) {
    impl_->ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(impl_->ctx, EVP_aes_128_ecb(), nullptr, key, nullptr);
    EVP_CIPHER_CTX_set_padding(impl_->ctx, 0);
}
Aes128::~Aes128() { EVP_CIPHER_CTX_free(impl_->ctx); }
void Aes128::encrypt_block(const uint8_t in[16], uint8_t out[16]) {
    int len = 0;
    EVP_EncryptUpdate(impl_->ctx, out, &len, in, 16);
}

#endif

void audio_decrypt(Aes128& aes, uint64_t offset, uint8_t* data, size_t len) {
    // librespot's AUDIO_AESIV: the counter for block 0, counting up big-endian by one a block.
    static const uint8_t IV[16] = {0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77,
                                   0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93};
    uint8_t ctr[16], stream[16];
    std::memcpy(ctr, IV, 16);
    uint64_t block = offset / 16;
    unsigned carry = 0;
    for (int i = 15; i >= 0; i--) {
        unsigned v = ctr[i] + (unsigned)(block & 0xFF) + carry;
        ctr[i] = (uint8_t)v;
        carry = v >> 8;
        block >>= 8;
    }
    size_t at = offset % 16;
    while (len) {
        aes.encrypt_block(ctr, stream);
        for (int i = 15; i >= 0; --i)
            if (++ctr[i]) break;
        for (; at < 16 && len; at++, len--) *data++ ^= stream[at];
        at = 0;
    }
}

}  // namespace spotify::crypto
