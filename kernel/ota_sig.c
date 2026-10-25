/* An update image's signature. The contract is in kernel/include/kernel/ota_sig.h. */

#include "kernel/ota_sig.h"
#include "kernel/crypto.h"
#include "kernel/sha256.h"

static bool all_zero(const uint8_t *p, uint32_t n) {
    uint8_t acc = 0;
    for (uint32_t i = 0; i < n; i++) acc |= p[i];
    return acc == 0;
}

void ota_sig_key_id(const uint8_t pubkey[64], uint8_t id[OTA_KEY_ID_LEN]) {
    uint8_t d[SHA256_DIGEST_LEN];
    sha256(pubkey, 64, d);
    for (unsigned i = 0; i < OTA_KEY_ID_LEN; i++) id[i] = d[i];
}

ota_sig_result_t ota_sig_check(const ota_image_header_t *h, const uint8_t *pubkey) {
    if (!all_zero(h->sig_pad, sizeof h->sig_pad) || !all_zero(h->reserved, sizeof h->reserved))
        return OTA_SIG_RESULT_MALFORMED;
    if (h->sig_alg == OTA_SIG_NONE) {
        return all_zero(h->key_id, sizeof h->key_id) && all_zero(h->sig, sizeof h->sig)
             ? OTA_SIG_RESULT_UNSIGNED : OTA_SIG_RESULT_MALFORMED;
    }
    if (h->sig_alg != OTA_SIG_P256) return OTA_SIG_RESULT_MALFORMED;
    if (!pubkey) return OTA_SIG_RESULT_NO_KEY;

    uint8_t id[OTA_KEY_ID_LEN];
    ota_sig_key_id(pubkey, id);
    for (unsigned i = 0; i < OTA_KEY_ID_LEN; i++)
        if (id[i] != h->key_id[i]) return OTA_SIG_RESULT_OTHER_KEY;

    uint8_t digest[SHA256_DIGEST_LEN];
    sha256(h, OTA_SIGNED_LEN, digest);
    return p256_verify(h->sig, pubkey, digest, sizeof digest) == CRYPTO_OK
         ? OTA_SIG_RESULT_OK : OTA_SIG_RESULT_BAD;
}

const char *ota_sig_result_str(ota_sig_result_t r) {
    switch (r) {
        case OTA_SIG_RESULT_OK:        return "signed by this node's release key";
        case OTA_SIG_RESULT_UNSIGNED:  return "unsigned";
        case OTA_SIG_RESULT_NO_KEY:    return "signed, but this node has no release key to check it with";
        case OTA_SIG_RESULT_OTHER_KEY: return "signed by a key this node does not trust";
        case OTA_SIG_RESULT_BAD:       return "the signature does not verify";
        case OTA_SIG_RESULT_MALFORMED: return "malformed signature fields";
    }
    return "?";
}
