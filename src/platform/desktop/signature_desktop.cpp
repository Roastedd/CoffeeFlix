// Signature checks with OpenSSL's libcrypto: see platform.hpp.
#include "platform/platform.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>

namespace platform {

bool verify_signature(const std::string& public_key_pem, const uint8_t digest[32], const std::vector<uint8_t>& sig) {
    BIO* bio = BIO_new_mem_buf(public_key_pem.data(), (int)public_key_pem.size());
    EVP_PKEY* key = bio ? PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr) : nullptr;
    EVP_PKEY_CTX* ctx = key && EVP_PKEY_base_id(key) == EVP_PKEY_EC ? EVP_PKEY_CTX_new(key, nullptr) : nullptr;
    bool ok = ctx && EVP_PKEY_verify_init(ctx) == 1 && EVP_PKEY_CTX_set_signature_md(ctx, EVP_sha256()) == 1 &&
              EVP_PKEY_verify(ctx, sig.data(), sig.size(), digest, 32) == 1;
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(key);
    BIO_free(bio);
    return ok;
}

}  // namespace platform
