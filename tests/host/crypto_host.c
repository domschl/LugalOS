/* Host test of kernel/crypto.c and the vendored code under it
 * (third_party/crypto/): the on-device known-answer tests twice, once as a
 * board with an entropy source and once as one without, then random round
 * trips where every single-bit change to a ciphertext, tag, signature or hash
 * must be refused. Run under ASan/UBSan and valgrind by the Makefile. */
#include "kernel/crypto.h"
#include "kernel/sha256.h"
#include "shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* The harness decides whether this "board" has hardware entropy. With it,
 * random bytes come from the host's own source; key generation and signing
 * are then exercised for real. */
static bool g_hw_entropy = true;
bool random_is_hardware(void) { return g_hw_entropy; }
const char *random_source(void) { return g_hw_entropy ? "host /dev/urandom" : "none (test)"; }
void random_bytes(void *buf, uint32_t len) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(buf, 1, len, f) != len) { fprintf(stderr, "no /dev/urandom\n"); exit(2); }
    fclose(f);
}

/* xorshift64*: the harness's own reproducible stream, seeded from argv. */
static uint64_t g_state;
static uint64_t next(void) {
    g_state ^= g_state >> 12; g_state ^= g_state << 25; g_state ^= g_state >> 27;
    return g_state * 0x2545F4914F6CDD1DULL;
}
static void fill(uint8_t *p, uint32_t n) { for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)next(); }

static void aead_round_trips(unsigned iters) {
    uint8_t key[32], nonce[12], ad[40], pt[300], ct[300], back[300], tag[16];
    for (unsigned it = 0; it < iters; it++) {
        uint32_t len = (uint32_t)(next() % sizeof(pt)), ad_len = (uint32_t)(next() % sizeof(ad));
        fill(key, sizeof key); fill(nonce, sizeof nonce); fill(ad, ad_len); fill(pt, len);
        chachapoly_seal(ct, tag, key, nonce, ad, ad_len, pt, len);
        CHECK(chachapoly_open(back, key, nonce, ad, ad_len, ct, len, tag) == CRYPTO_OK &&
              memcmp(back, pt, len) == 0, "aead round trip, len %u ad %u", len, ad_len);

        /* One bit anywhere in ct | ad | tag | nonce. */
        uint32_t total = len + ad_len + 16 + 12;
        uint32_t pos = (uint32_t)(next() % total);
        uint8_t bit = (uint8_t)(1u << (next() % 8));
        uint8_t *p = pos < len ? &ct[pos]
                   : pos < len + ad_len ? &ad[pos - len]
                   : pos < len + ad_len + 16 ? &tag[pos - len - ad_len]
                   : &nonce[pos - len - ad_len - 16];
        *p ^= bit;
        CHECK(chachapoly_open(back, key, nonce, ad, ad_len, ct, len, tag) == CRYPTO_ERR_VERIFY,
              "aead accepted a flipped bit at %u of %u", pos, total);
        *p ^= bit;
    }
}

static void x25519_round_trips(unsigned iters) {
    uint8_t s1[32], p1[32], s2[32], p2[32], z1[32], z2[32];
    for (unsigned it = 0; it < iters; it++) {
        fill(s1, 32); fill(s2, 32);
        x25519_public(p1, s1); x25519_public(p2, s2);
        CHECK(x25519_shared(z1, s1, p2) == CRYPTO_OK && x25519_shared(z2, s2, p1) == CRYPTO_OK &&
              memcmp(z1, z2, 32) == 0, "x25519 sides disagree");
    }
}

static void p256_round_trips(unsigned iters) {
    uint8_t priv[32], pub[64], sig[64], hash[32], priv2[32], pub2[64], z1[32], z2[32];
    for (unsigned it = 0; it < iters; it++) {
        CHECK(p256_keypair(priv, pub) == CRYPTO_OK, "keypair");
        fill(hash, 32);
        CHECK(p256_sign(sig, priv, hash, 32) == CRYPTO_OK, "sign");
        CHECK(p256_verify(sig, pub, hash, 32) == CRYPTO_OK, "own signature does not verify");

        uint32_t pos = (uint32_t)(next() % (64 + 32));
        uint8_t bit = (uint8_t)(1u << (next() % 8));
        uint8_t *p = pos < 64 ? &sig[pos] : &hash[pos - 64];
        *p ^= bit;
        int rc = p256_verify(sig, pub, hash, 32);
        CHECK(rc != CRYPTO_OK, "p256 accepted a flipped bit at %u", pos);
        *p ^= bit;

        CHECK(p256_keypair(priv2, pub2) == CRYPTO_OK, "keypair 2");
        CHECK(p256_ecdh(z1, priv, pub2) == CRYPTO_OK && p256_ecdh(z2, priv2, pub) == CRYPTO_OK &&
              memcmp(z1, z2, 32) == 0, "ecdh sides disagree");

        /* Random bytes as a public key: almost never on the curve, and must
         * then be refused rather than used. */
        uint8_t junk[64];
        fill(junk, 64);
        rc = p256_verify(sig, junk, hash, 32);
        CHECK(rc == CRYPTO_ERR_BAD_KEY || rc == CRYPTO_ERR_VERIFY, "junk public key: rc %d", rc);
    }
}

static void hash_kdf_properties(unsigned iters) {
    uint8_t msg[500], a[20], b[20], prk[32], okm1[100], okm2[100];
    for (unsigned it = 0; it < iters; it++) {
        uint32_t len = (uint32_t)(next() % sizeof msg), cut = len ? (uint32_t)(next() % len) : 0;
        fill(msg, len);
        sha1(msg, len, a);
        sha1_ctx_t c;
        sha1_init(&c); sha1_update(&c, msg, cut); sha1_update(&c, msg + cut, len - cut); sha1_final(&c, b);
        CHECK(memcmp(a, b, 20) == 0, "sha1 split at %u of %u differs", cut, len);

        hkdf_sha256_extract(msg, cut, msg + cut, len - cut, prk);
        uint32_t l1 = (uint32_t)(next() % sizeof okm1), l2 = (uint32_t)(next() % sizeof okm2);
        hkdf_sha256_expand(prk, "info", 4, okm1, l1);
        hkdf_sha256_expand(prk, "info", 4, okm2, l2);
        uint32_t l = l1 < l2 ? l1 : l2;
        CHECK(memcmp(okm1, okm2, l) == 0, "hkdf output of %u is not a prefix of %u", l1, l2);
    }
}

int main(int argc, char **argv) {
    unsigned iters = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 2000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x40c0ffee;
    g_state = seed ? seed : 1;

    g_hw_entropy = true;
    CHECK(crypto_selftest() == 0, "selftest with entropy");
    g_hw_entropy = false;
    CHECK(crypto_selftest() == 0, "selftest without entropy");
    g_hw_entropy = true;

    aead_round_trips(iters);
    x25519_round_trips(iters / 10 + 1);
    p256_round_trips(iters / 20 + 1);
    hash_kdf_properties(iters);

    if (fails) { printf("%d failures (seed %#llx)\n", fails, (unsigned long long)seed); return 1; }
    printf("crypto_host: %u AEAD and %u P-256 round trips with tampering (seed %llu), no fault\n",
           iters, iters / 20 + 1, (unsigned long long)seed);
    return 0;
}
