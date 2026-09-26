// Signature checks with mbedtls, which the Wii U build already links for TLS: see platform.hpp.
#include "platform/platform.hpp"

#include <mbedtls/pk.h>

namespace platform {

bool verify_signature(const std::string& public_key_pem, const uint8_t digest[32], const std::vector<uint8_t>& sig) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    // A PEM key's length counts its terminating zero.
    bool ok = mbedtls_pk_parse_public_key(&pk, (const unsigned char*)public_key_pem.c_str(), public_key_pem.size() + 1) == 0 &&
              mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECKEY) &&
              mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest, 32, sig.data(), sig.size()) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

}  // namespace platform
