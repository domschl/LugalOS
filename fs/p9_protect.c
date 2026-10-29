#include "fs/p9_protect.h"
#include "kernel/sha256.h"
#include <string.h>

static const char INFO_PREFIX[] = "LugalOS 9P v1";

void p9_protect_derive(const uint8_t *key, uint32_t key_len, const uint8_t nonce[32],
                       const char *uname, const char *aname, uint8_t out[P9_SESSION_KEY_LEN]) {
    uint8_t prk[SHA256_DIGEST_LEN];
    hmac_sha256(nonce, 32, key, key_len, prk);

    uint8_t info[sizeof INFO_PREFIX + 2 * 32 + 3];
    uint32_t n = 0;
    memcpy(info, INFO_PREFIX, sizeof INFO_PREFIX - 1);
    n += (uint32_t)sizeof INFO_PREFIX - 1;
    info[n++] = 0;
    for (const char *p = uname; *p && n < sizeof info - 34; p++) info[n++] = (uint8_t)*p;
    info[n++] = 0;
    for (const char *p = aname; *p && n < sizeof info - 2; p++) info[n++] = (uint8_t)*p;
    info[n++] = 0;
    info[n++] = 1;
    hmac_sha256(prk, sizeof prk, info, n, out);
    memset(prk, 0, sizeof prk);
}

void p9_protect_tag(const uint8_t key[P9_SESSION_KEY_LEN], uint8_t dir, uint32_t seq,
                    const uint8_t *frame, uint32_t len, uint8_t out[P9_TAG_LEN]) {
    uint8_t ipad[SHA256_BLOCK_LEN], opad[SHA256_BLOCK_LEN];
    memset(ipad, 0x36, sizeof ipad);
    memset(opad, 0x5c, sizeof opad);
    for (uint32_t i = 0; i < P9_SESSION_KEY_LEN; i++) { ipad[i] ^= key[i]; opad[i] ^= key[i]; }

    uint8_t head[5] = { dir, (uint8_t)seq, (uint8_t)(seq >> 8), (uint8_t)(seq >> 16), (uint8_t)(seq >> 24) };
    uint8_t inner[SHA256_DIGEST_LEN], mac[SHA256_DIGEST_LEN];
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, ipad, sizeof ipad);
    sha256_update(&c, head, sizeof head);
    sha256_update(&c, frame, len);
    sha256_final(&c, inner);
    sha256_init(&c);
    sha256_update(&c, opad, sizeof opad);
    sha256_update(&c, inner, sizeof inner);
    sha256_final(&c, mac);
    memcpy(out, mac, P9_TAG_LEN);
    memset(ipad, 0, sizeof ipad);
    memset(opad, 0, sizeof opad);
    memset(inner, 0, sizeof inner);
    memset(mac, 0, sizeof mac);
}

bool p9_protect_verify(const uint8_t key[P9_SESSION_KEY_LEN], uint8_t dir, uint32_t seq,
                       const uint8_t *frame, uint32_t len, const uint8_t tag[P9_TAG_LEN]) {
    uint8_t want[P9_TAG_LEN];
    p9_protect_tag(key, dir, seq, frame, len, want);
    bool ok = sha256_verify(want, tag, P9_TAG_LEN);
    memset(want, 0, sizeof want);
    return ok;
}
