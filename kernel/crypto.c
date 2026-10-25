/*
 * SHA-1, HKDF-SHA-256, and the wrappers over the vendored curve and stream
 * cipher code. The contract is in kernel/include/kernel/crypto.h.
 */

#include "kernel/crypto.h"
#include "kernel/sha256.h"
#include "kernel/random.h"

#include "third_party/crypto/monocypher/monocypher.h"
#include "third_party/crypto/p256-m/p256-m.h"

#include <string.h>

/* --- helpers ------------------------------------------------------------------ */

bool crypto_equal(const void *a, const void *b, uint32_t len) {
    return sha256_verify(a, b, len);
}

void crypto_wipe_bytes(void *p, uint32_t len) {
    crypto_wipe(p, len);
}

/* --- SHA-1 -------------------------------------------------------------------- */

static inline uint32_t rotl(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32u - n));
}

static void sha1_compress(sha1_ctx_t *ctx, const uint8_t block[64]) {
    /* A rolling 16-word schedule, as sha256_compress() does, to keep this off
     * the stack of whichever protocol task calls it. */
    uint32_t W[16];
    for (unsigned t = 0; t < 16; t++) {
        W[t] = ((uint32_t)block[t * 4] << 24) | ((uint32_t)block[t * 4 + 1] << 16) |
               ((uint32_t)block[t * 4 + 2] << 8) | (uint32_t)block[t * 4 + 3];
    }
    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3], e = ctx->h[4];
    for (unsigned t = 0; t < 80; t++) {
        if (t >= 16) {
            W[t & 15u] = rotl(W[(t + 13u) & 15u] ^ W[(t + 8u) & 15u] ^
                              W[(t + 2u) & 15u] ^ W[t & 15u], 1);
        }
        uint32_t f, k;
        if (t < 20)      { f = (b & c) | (~b & d);           k = 0x5a827999u; }
        else if (t < 40) { f = b ^ c ^ d;                    k = 0x6ed9eba1u; }
        else if (t < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8f1bbcdcu; }
        else             { f = b ^ c ^ d;                    k = 0xca62c1d6u; }
        uint32_t tmp = rotl(a, 5) + f + e + k + W[t & 15u];
        e = d; d = c; c = rotl(b, 30); b = a; a = tmp;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d; ctx->h[4] += e;
}

void sha1_init(sha1_ctx_t *ctx) {
    ctx->h[0] = 0x67452301u; ctx->h[1] = 0xefcdab89u; ctx->h[2] = 0x98badcfeu;
    ctx->h[3] = 0x10325476u; ctx->h[4] = 0xc3d2e1f0u;
    ctx->bits = 0;
    ctx->block_len = 0;
}

void sha1_update(sha1_ctx_t *ctx, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    ctx->bits += (uint64_t)len * 8u;
    while (len > 0) {
        uint32_t space = 64u - ctx->block_len;
        uint32_t take = (len < space) ? len : space;
        memcpy(ctx->block + ctx->block_len, p, take);
        ctx->block_len += take;
        p += take;
        len -= take;
        if (ctx->block_len == 64u) {
            sha1_compress(ctx, ctx->block);
            ctx->block_len = 0;
        }
    }
}

void sha1_final(sha1_ctx_t *ctx, uint8_t out[SHA1_DIGEST_LEN]) {
    uint64_t bits = ctx->bits;
    ctx->block[ctx->block_len++] = 0x80;
    if (ctx->block_len > 56u) {
        while (ctx->block_len < 64u) ctx->block[ctx->block_len++] = 0;
        sha1_compress(ctx, ctx->block);
        ctx->block_len = 0;
    }
    while (ctx->block_len < 56u) ctx->block[ctx->block_len++] = 0;
    for (int i = 7; i >= 0; i--) ctx->block[ctx->block_len++] = (uint8_t)(bits >> (i * 8));
    sha1_compress(ctx, ctx->block);
    for (unsigned i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)ctx->h[i];
    }
}

void sha1(const void *data, uint32_t len, uint8_t out[SHA1_DIGEST_LEN]) {
    sha1_ctx_t ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, out);
}

/* --- HKDF-SHA-256 ----------------------------------------------------------- */

void hkdf_sha256_extract(const void *salt, uint32_t salt_len,
                         const void *ikm, uint32_t ikm_len,
                         uint8_t prk[HKDF_SHA256_PRK_LEN]) {
    static const uint8_t zeros[32];
    if (!salt || salt_len == 0) { salt = zeros; salt_len = sizeof(zeros); }
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

/* T(i) = HMAC(PRK, T(i-1) | info | i). hmac_sha256() takes one message, so
 * each step is assembled in a small buffer and `info` is fed in chunks
 * through an HMAC built from sha256_ctx_t directly. */
static void hkdf_step(const uint8_t prk[32], const uint8_t *prev, uint32_t prev_len,
                      const void *info, uint32_t info_len, uint8_t counter,
                      uint8_t out[32]) {
    uint8_t pad[SHA256_BLOCK_LEN];
    sha256_ctx_t ctx;

    for (unsigned i = 0; i < SHA256_BLOCK_LEN; i++) pad[i] = (uint8_t)((i < 32 ? prk[i] : 0) ^ 0x36);
    sha256_init(&ctx);
    sha256_update(&ctx, pad, sizeof(pad));
    sha256_update(&ctx, prev, prev_len);
    sha256_update(&ctx, info, info_len);
    sha256_update(&ctx, &counter, 1);
    uint8_t inner[32];
    sha256_final(&ctx, inner);

    for (unsigned i = 0; i < SHA256_BLOCK_LEN; i++) pad[i] = (uint8_t)((i < 32 ? prk[i] : 0) ^ 0x5c);
    sha256_init(&ctx);
    sha256_update(&ctx, pad, sizeof(pad));
    sha256_update(&ctx, inner, sizeof(inner));
    sha256_final(&ctx, out);

    crypto_wipe(pad, sizeof(pad));
    crypto_wipe(inner, sizeof(inner));
    crypto_wipe(&ctx, sizeof(ctx));
}

int hkdf_sha256_expand(const uint8_t prk[HKDF_SHA256_PRK_LEN],
                       const void *info, uint32_t info_len,
                       uint8_t *out, uint32_t out_len) {
    if (out_len > HKDF_SHA256_OUT_MAX) return CRYPTO_ERR_BAD_INPUT;
    if (!info) info_len = 0;
    uint8_t t[32];
    uint32_t t_len = 0;
    uint8_t counter = 1;
    while (out_len > 0) {
        hkdf_step(prk, t, t_len, info, info_len, counter++, t);
        t_len = 32;
        uint32_t take = out_len < 32u ? out_len : 32u;
        memcpy(out, t, take);
        out += take;
        out_len -= take;
    }
    crypto_wipe(t, sizeof(t));
    return CRYPTO_OK;
}

/* --- X25519 ------------------------------------------------------------------- */

int x25519_keypair(uint8_t secret[X25519_KEY_LEN], uint8_t pub[X25519_KEY_LEN]) {
    if (!random_is_hardware()) return CRYPTO_ERR_NO_ENTROPY;
    random_bytes(secret, X25519_KEY_LEN);
    crypto_x25519_public_key(pub, secret);
    return CRYPTO_OK;
}

void x25519_public(uint8_t pub[X25519_KEY_LEN], const uint8_t secret[X25519_KEY_LEN]) {
    crypto_x25519_public_key(pub, secret);
}

int x25519_shared(uint8_t shared[X25519_KEY_LEN],
                  const uint8_t secret[X25519_KEY_LEN],
                  const uint8_t peer_pub[X25519_KEY_LEN]) {
    static const uint8_t zero[X25519_KEY_LEN];
    crypto_x25519(shared, secret, peer_pub);
    if (crypto_equal(shared, zero, X25519_KEY_LEN)) {
        crypto_wipe(shared, X25519_KEY_LEN);
        return CRYPTO_ERR_BAD_KEY;
    }
    return CRYPTO_OK;
}

/* --- ChaCha20-Poly1305 (RFC 8439 §2.8) ------------------------------------- */

/* The tag over ad | pad16 | ct | pad16 | le64(ad_len) | le64(ct_len), keyed
 * by the first 32 bytes of ChaCha20's block 0. */
static void chachapoly_tag(uint8_t tag[CHACHAPOLY_TAG_LEN],
                           const uint8_t key[CHACHAPOLY_KEY_LEN],
                           const uint8_t nonce[CHACHAPOLY_NONCE_LEN],
                           const void *ad, uint32_t ad_len,
                           const uint8_t *ct, uint32_t len) {
    static const uint8_t zeros[16];
    uint8_t block0[64];
    memset(block0, 0, sizeof(block0));
    crypto_chacha20_ietf(block0, block0, sizeof(block0), key, nonce, 0);

    crypto_poly1305_ctx ctx;
    crypto_poly1305_init(&ctx, block0);
    if (ad_len) crypto_poly1305_update(&ctx, (const uint8_t *)ad, ad_len);
    crypto_poly1305_update(&ctx, zeros, (16u - (ad_len & 15u)) & 15u);
    if (len) crypto_poly1305_update(&ctx, ct, len);
    crypto_poly1305_update(&ctx, zeros, (16u - (len & 15u)) & 15u);
    uint8_t lens[16];
    uint64_t a = ad_len, c = len;
    for (unsigned i = 0; i < 8; i++) {
        lens[i]     = (uint8_t)(a >> (8 * i));
        lens[8 + i] = (uint8_t)(c >> (8 * i));
    }
    crypto_poly1305_update(&ctx, lens, sizeof(lens));
    crypto_poly1305_final(&ctx, tag);
    crypto_wipe(block0, sizeof(block0));
}

void chachapoly_seal(uint8_t *ct, uint8_t tag[CHACHAPOLY_TAG_LEN],
                     const uint8_t key[CHACHAPOLY_KEY_LEN],
                     const uint8_t nonce[CHACHAPOLY_NONCE_LEN],
                     const void *ad, uint32_t ad_len,
                     const uint8_t *pt, uint32_t len) {
    if (len) crypto_chacha20_ietf(ct, pt, len, key, nonce, 1);
    chachapoly_tag(tag, key, nonce, ad, ad_len, ct, len);
}

int chachapoly_open(uint8_t *pt,
                    const uint8_t key[CHACHAPOLY_KEY_LEN],
                    const uint8_t nonce[CHACHAPOLY_NONCE_LEN],
                    const void *ad, uint32_t ad_len,
                    const uint8_t *ct, uint32_t len,
                    const uint8_t tag[CHACHAPOLY_TAG_LEN]) {
    uint8_t expect[CHACHAPOLY_TAG_LEN];
    chachapoly_tag(expect, key, nonce, ad, ad_len, ct, len);
    int ok = crypto_verify16(expect, tag) == 0;
    crypto_wipe(expect, sizeof(expect));
    if (!ok) return CRYPTO_ERR_VERIFY;
    if (len) crypto_chacha20_ietf(pt, ct, len, key, nonce, 1);
    return CRYPTO_OK;
}

/* --- P-256 ------------------------------------------------------------------- */

/* p256-m's one hook: key generation and the ECDSA nonce draw from here. The
 * wrappers below check random_is_hardware() before p256-m can reach it, so a
 * call that arrives here on a target without a source came from somewhere
 * that skipped the check: refuse it rather than hand out predictable bytes. */
int p256_generate_random(uint8_t *output, unsigned output_size) {
    if (!random_is_hardware()) return P256_RANDOM_FAILED;
    random_bytes(output, output_size);
    return P256_SUCCESS;
}

static int map_p256(int rc) {
    switch (rc) {
        case P256_SUCCESS:           return CRYPTO_OK;
        case P256_RANDOM_FAILED:     return CRYPTO_ERR_NO_ENTROPY;
        case P256_INVALID_SIGNATURE: return CRYPTO_ERR_VERIFY;
        default:                     return CRYPTO_ERR_BAD_KEY;
    }
}

int p256_keypair(uint8_t priv[P256_PRIV_LEN], uint8_t pub[P256_PUB_LEN]) {
    if (!random_is_hardware()) return CRYPTO_ERR_NO_ENTROPY;
    return map_p256(p256_gen_keypair(priv, pub));
}

int p256_sign(uint8_t sig[P256_SIG_LEN], const uint8_t priv[P256_PRIV_LEN],
              const uint8_t *hash, uint32_t hash_len) {
    if (!random_is_hardware()) return CRYPTO_ERR_NO_ENTROPY;
    return map_p256(p256_ecdsa_sign(sig, priv, hash, hash_len));
}

int p256_verify(const uint8_t sig[P256_SIG_LEN], const uint8_t pub[P256_PUB_LEN],
                const uint8_t *hash, uint32_t hash_len) {
    return map_p256(p256_ecdsa_verify(sig, pub, hash, hash_len));
}

/* p256-m validates a peer key on every ECDH (on the curve, coordinates
 * below p), so ECDH with the scalar 1 -- no secret involved -- is the check. */
bool p256_pubkey_valid(const uint8_t pub[P256_PUB_LEN]) {
    uint8_t one[P256_PRIV_LEN] = { 0 }, x[32];
    one[P256_PRIV_LEN - 1] = 1;
    return p256_ecdh_shared_secret(x, one, pub) == P256_SUCCESS;
}

int p256_ecdh(uint8_t shared[32], const uint8_t priv[P256_PRIV_LEN],
              const uint8_t peer_pub[P256_PUB_LEN]) {
    return map_p256(p256_ecdh_shared_secret(shared, priv, peer_pub));
}
