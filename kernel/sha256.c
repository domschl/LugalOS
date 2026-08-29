/*
 * SHA-256 and HMAC-SHA-256 — N1, plan/phase18_networking_and_auth.md.
 * See kernel/include/kernel/sha256.h for why this exists and what it is not.
 *
 * A direct transcription of FIPS 180-4 §6.2 and RFC 2104, chosen to be
 * checkable against the standard line by line rather than clever. There is
 * one deliberate deviation from the most obvious transcription, in the
 * message schedule -- see W[] below.
 */

#include "kernel/sha256.h"
#include "kernel/console.h"

#include <string.h>

/* First 32 bits of the fractional parts of the cube roots of the first 64
 * primes (FIPS 180-4 §4.2.2). */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (rotr((x),  2) ^ rotr((x), 13) ^ rotr((x), 22))
#define BSIG1(x) (rotr((x),  6) ^ rotr((x), 11) ^ rotr((x), 25))
#define SSIG0(x) (rotr((x),  7) ^ rotr((x), 18) ^ ((x) >>  3))
#define SSIG1(x) (rotr((x), 17) ^ rotr((x), 19) ^ ((x) >> 10))

static void sha256_compress(sha256_ctx_t *ctx, const uint8_t block[64]) {
    /* Sixteen words, not the sixty-four the standard's own notation suggests.
     * W[t] for t >= 16 depends only on the previous sixteen, so the schedule
     * can roll through a 16-word window: 64 bytes of stack instead of 256.
     *
     * That matters here specifically. This runs inside the 9P server task,
     * and phase 17b's U-mode work is a standing reminder that stacks in this
     * kernel are 4 KB and sometimes 1.5 KB -- 192 bytes is not nothing when
     * the call chain underneath is a protocol handler. The indexing is the
     * standard's, modulo 16. */
    uint32_t W[16];
    for (unsigned t = 0; t < 16; t++) {
        W[t] = ((uint32_t)block[t * 4] << 24) | ((uint32_t)block[t * 4 + 1] << 16) |
               ((uint32_t)block[t * 4 + 2] << 8) | (uint32_t)block[t * 4 + 3];
    }

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];

    for (unsigned t = 0; t < 64; t++) {
        if (t >= 16) {
            W[t & 15u] += SSIG1(W[(t + 14u) & 15u]) + W[(t + 9u) & 15u]
                        + SSIG0(W[(t + 1u) & 15u]);
        }
        uint32_t t1 = h + BSIG1(e) + CH(e, f, g) + K[t] + W[t & 15u];
        uint32_t t2 = BSIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void sha256_init(sha256_ctx_t *ctx) {
    /* First 32 bits of the fractional parts of the square roots of the first
     * eight primes (FIPS 180-4 §5.3.3). */
    ctx->h[0] = 0x6a09e667; ctx->h[1] = 0xbb67ae85;
    ctx->h[2] = 0x3c6ef372; ctx->h[3] = 0xa54ff53a;
    ctx->h[4] = 0x510e527f; ctx->h[5] = 0x9b05688c;
    ctx->h[6] = 0x1f83d9ab; ctx->h[7] = 0x5be0cd19;
    ctx->bits = 0;
    ctx->block_len = 0;
}

void sha256_update(sha256_ctx_t *ctx, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    ctx->bits += (uint64_t)len * 8u;

    while (len > 0) {
        uint32_t space = SHA256_BLOCK_LEN - ctx->block_len;
        uint32_t take = (len < space) ? len : space;
        for (uint32_t i = 0; i < take; i++) ctx->block[ctx->block_len + i] = p[i];
        ctx->block_len += take;
        p += take;
        len -= take;
        if (ctx->block_len == SHA256_BLOCK_LEN) {
            sha256_compress(ctx, ctx->block);
            ctx->block_len = 0;
        }
    }
}

void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_LEN]) {
    uint64_t bits = ctx->bits;

    /* 0x80, then zeros, then the 64-bit big-endian bit count in the last
     * eight bytes -- spilling into a second block when there is no room. */
    ctx->block[ctx->block_len++] = 0x80;
    if (ctx->block_len > SHA256_BLOCK_LEN - 8u) {
        while (ctx->block_len < SHA256_BLOCK_LEN) ctx->block[ctx->block_len++] = 0;
        sha256_compress(ctx, ctx->block);
        ctx->block_len = 0;
    }
    while (ctx->block_len < SHA256_BLOCK_LEN - 8u) ctx->block[ctx->block_len++] = 0;
    for (int i = 7; i >= 0; i--) ctx->block[ctx->block_len++] = (uint8_t)(bits >> (i * 8));
    sha256_compress(ctx, ctx->block);

    for (unsigned i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->h[i]);
    }
    /* Not decoration: the context holds one compression's worth of message
     * bytes, and for HMAC that is key material. */
    memset(ctx, 0, sizeof(*ctx));
}

void sha256(const void *data, uint32_t len, uint8_t out[SHA256_DIGEST_LEN]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

void hmac_sha256(const void *key, uint32_t key_len,
                 const void *msg, uint32_t msg_len,
                 uint8_t out[SHA256_DIGEST_LEN]) {
    uint8_t k[SHA256_BLOCK_LEN];
    uint8_t pad[SHA256_BLOCK_LEN];
    uint8_t inner[SHA256_DIGEST_LEN];
    sha256_ctx_t ctx;

    /* RFC 2104: a key longer than the block is replaced by its own hash;
     * anything shorter is zero-padded to the block length. */
    memset(k, 0, sizeof(k));
    if (key_len > SHA256_BLOCK_LEN) {
        sha256(key, key_len, k);
    } else {
        for (uint32_t i = 0; i < key_len; i++) k[i] = ((const uint8_t *)key)[i];
    }

    for (unsigned i = 0; i < SHA256_BLOCK_LEN; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&ctx);
    sha256_update(&ctx, pad, SHA256_BLOCK_LEN);
    sha256_update(&ctx, msg, msg_len);
    sha256_final(&ctx, inner);

    for (unsigned i = 0; i < SHA256_BLOCK_LEN; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&ctx);
    sha256_update(&ctx, pad, SHA256_BLOCK_LEN);
    sha256_update(&ctx, inner, SHA256_DIGEST_LEN);
    sha256_final(&ctx, out);

    /* The padded key and the inner digest are both key-derived. */
    memset(k, 0, sizeof(k));
    memset(pad, 0, sizeof(pad));
    memset(inner, 0, sizeof(inner));
}

void key_fingerprint_hex(const void *key, uint32_t key_len, char out[KEY_FINGERPRINT_HEX_LEN + 1]) {
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[SHA256_DIGEST_LEN];
    sha256(key, key_len, digest);
    for (unsigned i = 0; i < KEY_FINGERPRINT_HEX_LEN / 2; i++) {
        out[i * 2]     = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[KEY_FINGERPRINT_HEX_LEN] = '\0';
    memset(digest, 0, sizeof(digest));
}

bool sha256_verify(const void *a, const void *b, uint32_t len) {
    const volatile uint8_t *x = (const volatile uint8_t *)a;
    const volatile uint8_t *y = (const volatile uint8_t *)b;
    /* Accumulate every difference, and never branch on one: the loop runs the
     * same number of times whatever the data is. volatile so the compiler
     * cannot decide it may as well stop early. */
    uint8_t diff = 0;
    for (uint32_t i = 0; i < len; i++) diff |= (uint8_t)(x[i] ^ y[i]);
    return diff == 0;
}

/* ------------------------------------------------- known-answer tests --- */

static bool digest_is(const uint8_t got[SHA256_DIGEST_LEN], const char *want_hex) {
    for (unsigned i = 0; i < SHA256_DIGEST_LEN; i++) {
        char hi = want_hex[i * 2], lo = want_hex[i * 2 + 1];
        unsigned v = (unsigned)((hi <= '9' ? hi - '0' : hi - 'a' + 10) << 4)
                   | (unsigned)(lo <= '9' ? lo - '0' : lo - 'a' + 10);
        if (got[i] != (uint8_t)v) return false;
    }
    return true;
}

static void report(const char *what, bool ok, const uint8_t got[SHA256_DIGEST_LEN],
                   const char *want_hex, int *failures) {
    cprintf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        (*failures)++;
        cprintf("        got  ");
        for (unsigned i = 0; i < SHA256_DIGEST_LEN; i++) cprintf("%02x", got[i]);
        cprintf("\n        want %s\n", want_hex);
    }
}

int sha256_selftest(void) {
    int failures = 0;
    uint8_t out[SHA256_DIGEST_LEN];

    cprintf("SHA-256 / HMAC-SHA-256 selftest:\n");

    /* --- SHA-256, FIPS 180-4's own examples ---------------------------- */
    {
        const char *want = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        sha256("", 0, out);
        report("SHA-256 of the empty string", digest_is(out, want), out, want, &failures);
    }
    {
        const char *want = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        sha256("abc", 3, out);
        report("SHA-256(\"abc\")", digest_is(out, want), out, want, &failures);
    }
    {
        /* 448 bits: the case that exercises the padding spilling into a
         * second block, which is where a hand-written final() usually goes
         * wrong. */
        const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        const char *want = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
        sha256(msg, 56, out);
        report("SHA-256 of a 448-bit message (padding spills a block)",
               digest_is(out, want), out, want, &failures);
    }
    {
        /* Fed one byte at a time, to prove the streaming path and the
         * one-shot path agree -- the auth exchange uses update() twice. */
        const char *want = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        sha256_ctx_t ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, "a", 1);
        sha256_update(&ctx, "b", 1);
        sha256_update(&ctx, "c", 1);
        sha256_final(&ctx, out);
        report("streamed byte at a time == one-shot", digest_is(out, want), out, want, &failures);
    }

    /* --- HMAC-SHA-256, RFC 4231 --------------------------------------- */
    {
        uint8_t key[20];
        memset(key, 0x0b, sizeof(key));
        const char *want = "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7";
        hmac_sha256(key, sizeof(key), "Hi There", 8, out);
        report("RFC 4231 case 1 (20-byte key)", digest_is(out, want), out, want, &failures);
    }
    {
        const char *want = "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843";
        hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
        report("RFC 4231 case 2 (short key)", digest_is(out, want), out, want, &failures);
    }
    {
        uint8_t key[20], msg[50];
        memset(key, 0xaa, sizeof(key));
        memset(msg, 0xdd, sizeof(msg));
        const char *want = "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe";
        hmac_sha256(key, sizeof(key), msg, sizeof(msg), out);
        report("RFC 4231 case 3 (50-byte message)", digest_is(out, want), out, want, &failures);
    }
    {
        uint8_t key[25], msg[50];
        for (unsigned i = 0; i < sizeof(key); i++) key[i] = (uint8_t)(i + 1);
        memset(msg, 0xcd, sizeof(msg));
        const char *want = "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b";
        hmac_sha256(key, sizeof(key), msg, sizeof(msg), out);
        report("RFC 4231 case 4 (25-byte key)", digest_is(out, want), out, want, &failures);
    }
    {
        /* The two cases with a key LONGER than the 64-byte block, which is
         * the branch of RFC 2104 that hashes the key first -- and the one a
         * hand-written HMAC most often gets wrong, because every short-key
         * test passes without it. */
        uint8_t key[131];
        memset(key, 0xaa, sizeof(key));
        const char *msg6 = "Test Using Larger Than Block-Size Key - Hash Key First";
        const char *want6 = "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54";
        hmac_sha256(key, sizeof(key), msg6, 54, out);
        report("RFC 4231 case 6 (131-byte key, hashed first)",
               digest_is(out, want6), out, want6, &failures);

        const char *msg7 =
            "This is a test using a larger than block-size key and a larger "
            "than block-size data. The key needs to be hashed before being "
            "used by the HMAC algorithm.";
        const char *want7 = "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2";
        hmac_sha256(key, sizeof(key), msg7, 152, out);
        report("RFC 4231 case 7 (long key and long message)",
               digest_is(out, want7), out, want7, &failures);
    }

    /* --- the comparison the auth path will actually use ---------------- */
    {
        uint8_t a[SHA256_DIGEST_LEN], b[SHA256_DIGEST_LEN];
        memset(a, 0x5a, sizeof(a));
        memset(b, 0x5a, sizeof(b));
        bool ok = sha256_verify(a, b, sizeof(a));
        b[SHA256_DIGEST_LEN - 1] ^= 0x01;          /* differs in the LAST byte */
        ok = ok && !sha256_verify(a, b, sizeof(a));
        b[SHA256_DIGEST_LEN - 1] ^= 0x01;
        b[0] ^= 0x80;                              /* and in the FIRST */
        ok = ok && !sha256_verify(a, b, sizeof(a));
        cprintf("  [%s] constant-time verify accepts equal, rejects either end\n",
                ok ? "ok" : "FAIL");
        if (!ok) failures++;
    }

    if (failures == 0) cprintf("HMAC_SELFTEST_OK (11/11)\n");
    else               cprintf("HMAC_SELFTEST_FAIL (%d failed)\n", failures);
    return failures;
}
