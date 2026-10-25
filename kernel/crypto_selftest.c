/*
 * `cryptotest`: known-answer tests and timings for kernel/crypto.c.
 *
 * Every vector is the standard's own (FIPS 180-4 / RFC 3174, RFC 6455,
 * RFC 5869, RFC 7748, RFC 8439, RFC 6979 A.2.5), cross-checked against an
 * independent implementation when it was added. The P-256 ECDH pair uses
 * RFC 6979's key and a second key, d2 = SHA-256("lugalos ecdh test key"),
 * with the shared secret computed by that same independent implementation.
 */

#include "kernel/crypto.h"
#include "kernel/sha256.h"
#include "kernel/random.h"
#include "kernel/console.h"
#include "kernel/time.h"

#include <string.h>

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes `hex` (exactly 2*len digits) into `out`. */
static void unhex(uint8_t *out, uint32_t len, const char *hex) {
    for (uint32_t i = 0; i < len; i++)
        out[i] = (uint8_t)((hexval(hex[2 * i]) << 4) | hexval(hex[2 * i + 1]));
}

static bool bytes_are(const uint8_t *got, uint32_t len, const char *want_hex) {
    if (strlen(want_hex) != 2u * len) return false;
    for (uint32_t i = 0; i < len; i++) {
        int hi = hexval(want_hex[2 * i]), lo = hexval(want_hex[2 * i + 1]);
        if (hi < 0 || lo < 0 || got[i] != (uint8_t)((hi << 4) | lo)) return false;
    }
    return true;
}

static void check(const char *what, bool ok, int *failures) {
    cprintf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) (*failures)++;
}

static void check_bytes(const char *what, const uint8_t *got, uint32_t len,
                        const char *want_hex, int *failures) {
    bool ok = bytes_are(got, len, want_hex);
    check(what, ok, failures);
    if (!ok) {
        cprintf("        got  ");
        for (uint32_t i = 0; i < len; i++) cprintf("%02x", got[i]);
        cprintf("\n        want %s\n", want_hex);
    }
}

static void skip(const char *what) {
    cprintf("  [skip] %s (no hardware entropy)\n", what);
}

/* --- vectors ----------------------------------------------------------------- */

#define RFC7748_ALICE_PRIV "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"
#define RFC7748_ALICE_PUB  "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"
#define RFC7748_BOB_PRIV   "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb"
#define RFC7748_BOB_PUB    "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"
#define RFC7748_SHARED     "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"

#define RFC8439_PT "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it."
#define RFC8439_CT "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6" \
                   "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36" \
                   "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc" \
                   "3ff4def08e4b7a9de576d26586cec64b6116"
#define RFC8439_TAG "1ae10b594f09e26a7e902ecbd0600691"

#define RFC6979_PRIV  "c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721"
#define RFC6979_PUB   "60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6" \
                      "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299"
#define RFC6979_SIG_SAMPLE "efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716" \
                           "f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8"
#define RFC6979_SIG_TEST   "f1abb023518351cd71d881567b1ea663ed3efcf6c5132b354f28d3b0b7d38367" \
                           "019f4113742a2b14bd25926b49c649155f267e60d3814b4c0cc84250e46f0083"
#define ECDH_D2      "d73bbe90b31a9857764287b6fc36b1dd497ce3abb17fd0c33ed0b0f3ac76a9d9"
#define ECDH_P2      "77f6ac4163874bbe5ecbeec785e93001c13082dd336b6ae9b00aadb5b65e2919" \
                     "0bac01cf611dcea89ffa86ad909eb0bda16a7f6570ac5f624d8be5db1bb21b73"
#define ECDH_SHARED  "ef7ae56797fd293f68bfced754a0ff035c6528f01ad92683886c9f225b42f85b"

static void test_sha1(int *failures) {
    uint8_t out[SHA1_DIGEST_LEN];
    sha1("", 0, out);
    check_bytes("SHA-1 of the empty string", out, sizeof(out),
                "da39a3ee5e6b4b0d3255bfef95601890afd80709", failures);
    sha1("abc", 3, out);
    check_bytes("SHA-1(\"abc\")", out, sizeof(out),
                "a9993e364706816aba3e25717850c26c9cd0d89d", failures);
    sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, out);
    check_bytes("SHA-1 of a 448-bit message (padding spills a block)", out, sizeof(out),
                "84983e441c3bd26ebaae4aa1f95129e5e54670f1", failures);

    sha1_ctx_t ctx;
    uint8_t chunk[200];
    memset(chunk, 'a', sizeof(chunk));
    sha1_init(&ctx);
    for (unsigned i = 0; i < 5000; i++) sha1_update(&ctx, chunk, sizeof(chunk));
    sha1_final(&ctx, out);
    check_bytes("SHA-1 of one million 'a', streamed", out, sizeof(out),
                "34aa973cd4c4daa4f61eeb2bdbad27316534016f", failures);

    /* RFC 6455 sec. 1.3's handshake example: the key plus the protocol's GUID. */
    const char *ws = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    sha1(ws, (uint32_t)strlen(ws), out);
    check_bytes("SHA-1 of RFC 6455's Sec-WebSocket-Key example", out, sizeof(out),
                "b37a4f2cc0624f1690f64606cf385945b2bec4ea", failures);
}

static void test_hkdf(int *failures) {
    uint8_t ikm[22], salt[13], info[10], prk[32], okm[42];
    memset(ikm, 0x0b, sizeof(ikm));
    for (unsigned i = 0; i < sizeof(salt); i++) salt[i] = (uint8_t)i;
    for (unsigned i = 0; i < sizeof(info); i++) info[i] = (uint8_t)(0xf0 + i);

    hkdf_sha256_extract(salt, sizeof(salt), ikm, sizeof(ikm), prk);
    check_bytes("HKDF-SHA-256 RFC 5869 case 1, PRK", prk, sizeof(prk),
                "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", failures);
    int rc = hkdf_sha256_expand(prk, info, sizeof(info), okm, sizeof(okm));
    check_bytes("HKDF-SHA-256 RFC 5869 case 1, OKM (42 bytes, two blocks)", okm, sizeof(okm),
                "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
                failures);
    if (rc != CRYPTO_OK) check("HKDF expand returned OK", false, failures);

    hkdf_sha256_extract(NULL, 0, ikm, sizeof(ikm), prk);
    check_bytes("HKDF-SHA-256 RFC 5869 case 3 (no salt, no info), PRK", prk, sizeof(prk),
                "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04", failures);
    hkdf_sha256_expand(prk, NULL, 0, okm, sizeof(okm));
    check_bytes("HKDF-SHA-256 RFC 5869 case 3, OKM", okm, sizeof(okm),
                "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8",
                failures);

    check("HKDF expand refuses more than 255 blocks",
          hkdf_sha256_expand(prk, NULL, 0, okm, HKDF_SHA256_OUT_MAX + 1u) == CRYPTO_ERR_BAD_INPUT,
          failures);
}

static void test_x25519(int *failures) {
    uint8_t k[32], u[32], out[32], a[32], b[32], pa[32], pb[32];

    unhex(k, 32, "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
    unhex(u, 32, "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
    x25519_shared(out, k, u);
    check_bytes("X25519 RFC 7748 sec. 5.2 scalar multiplication", out, 32,
                "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", failures);

    unhex(a, 32, RFC7748_ALICE_PRIV);
    unhex(b, 32, RFC7748_BOB_PRIV);
    x25519_public(pa, a);
    check_bytes("X25519 RFC 7748 sec. 6.1 Alice's public key", pa, 32, RFC7748_ALICE_PUB, failures);
    x25519_public(pb, b);
    check_bytes("X25519 RFC 7748 sec. 6.1 Bob's public key", pb, 32, RFC7748_BOB_PUB, failures);
    x25519_shared(out, a, pb);
    check_bytes("X25519 RFC 7748 sec. 6.1 shared secret, Alice's side", out, 32, RFC7748_SHARED, failures);
    x25519_shared(out, b, pa);
    check_bytes("X25519 RFC 7748 sec. 6.1 shared secret, Bob's side", out, 32, RFC7748_SHARED, failures);

    memset(u, 0, sizeof(u));
    check("X25519 refuses a low-order peer key (all-zero result)",
          x25519_shared(out, a, u) == CRYPTO_ERR_BAD_KEY, failures);

    if (random_is_hardware()) {
        uint8_t s1[32], p1[32], s2[32], p2[32], z1[32], z2[32];
        bool ok = x25519_keypair(s1, p1) == CRYPTO_OK && x25519_keypair(s2, p2) == CRYPTO_OK &&
                  x25519_shared(z1, s1, p2) == CRYPTO_OK && x25519_shared(z2, s2, p1) == CRYPTO_OK &&
                  crypto_equal(z1, z2, 32) && !crypto_equal(s1, s2, 32);
        check("X25519 fresh key pairs agree on a shared secret", ok, failures);
        crypto_wipe_bytes(s1, 32); crypto_wipe_bytes(s2, 32);
    } else {
        check("X25519 key generation refuses without hardware entropy",
              x25519_keypair(a, pa) == CRYPTO_ERR_NO_ENTROPY, failures);
        skip("X25519 fresh key pairs");
    }
}

static void test_chachapoly(int *failures) {
    uint8_t key[32], nonce[12], ad[12], tag[16], buf[114], back[114];
    for (unsigned i = 0; i < 32; i++) key[i] = (uint8_t)(0x80 + i);
    unhex(nonce, 12, "070000004041424344454647");
    unhex(ad, 12, "50515253c0c1c2c3c4c5c6c7");
    const uint8_t *pt = (const uint8_t *)RFC8439_PT;

    chachapoly_seal(buf, tag, key, nonce, ad, sizeof(ad), pt, sizeof(buf));
    check_bytes("ChaCha20-Poly1305 RFC 8439 sec. 2.8.2 ciphertext", buf, sizeof(buf), RFC8439_CT, failures);
    check_bytes("ChaCha20-Poly1305 RFC 8439 sec. 2.8.2 tag", tag, sizeof(tag), RFC8439_TAG, failures);

    memset(back, 0, sizeof(back));
    check("ChaCha20-Poly1305 opens its own output",
          chachapoly_open(back, key, nonce, ad, sizeof(ad), buf, sizeof(buf), tag) == CRYPTO_OK &&
          memcmp(back, pt, sizeof(back)) == 0, failures);

    memset(back, 0x5a, sizeof(back));
    buf[17] ^= 0x01;
    bool refused = chachapoly_open(back, key, nonce, ad, sizeof(ad), buf, sizeof(buf), tag) == CRYPTO_ERR_VERIFY;
    bool untouched = true;
    for (unsigned i = 0; i < sizeof(back); i++) if (back[i] != 0x5a) untouched = false;
    check("a flipped ciphertext bit is refused, and nothing is decrypted", refused && untouched, failures);
    buf[17] ^= 0x01;

    ad[0] ^= 0x80;
    check("a changed associated data byte is refused",
          chachapoly_open(back, key, nonce, ad, sizeof(ad), buf, sizeof(buf), tag) == CRYPTO_ERR_VERIFY,
          failures);
    ad[0] ^= 0x80;
    tag[15] ^= 0x01;
    check("a changed tag is refused",
          chachapoly_open(back, key, nonce, ad, sizeof(ad), buf, sizeof(buf), tag) == CRYPTO_ERR_VERIFY,
          failures);
    tag[15] ^= 0x01;

    /* In place, as TLS records are: the same buffer in and out. */
    memcpy(back, pt, sizeof(back));
    chachapoly_seal(back, tag, key, nonce, ad, sizeof(ad), back, sizeof(back));
    bool sealed = bytes_are(back, sizeof(back), RFC8439_CT);
    bool opened = chachapoly_open(back, key, nonce, ad, sizeof(ad), back, sizeof(back), tag) == CRYPTO_OK &&
                  memcmp(back, pt, sizeof(back)) == 0;
    check("ChaCha20-Poly1305 in place (one buffer for input and output)", sealed && opened, failures);

    chachapoly_seal(buf, tag, key, nonce, NULL, 0, NULL, 0);
    check("ChaCha20-Poly1305 with no data and no AD seals and opens",
          chachapoly_open(buf, key, nonce, NULL, 0, buf, 0, tag) == CRYPTO_OK, failures);
}

static void test_p256(int *failures) {
    uint8_t priv[32], pub[64], sig[64], hash[32], d2[32], p2[64], shared[32];
    unhex(priv, 32, RFC6979_PRIV);
    unhex(pub, 64, RFC6979_PUB);

    sha256("sample", 6, hash);
    unhex(sig, 64, RFC6979_SIG_SAMPLE);
    check("P-256 verifies RFC 6979 A.2.5's signature of \"sample\"",
          p256_verify(sig, pub, hash, 32) == CRYPTO_OK, failures);
    sha256("test", 4, hash);
    unhex(sig, 64, RFC6979_SIG_TEST);
    check("P-256 verifies RFC 6979 A.2.5's signature of \"test\"",
          p256_verify(sig, pub, hash, 32) == CRYPTO_OK, failures);

    hash[0] ^= 0x01;
    check("P-256 refuses the signature over a different hash",
          p256_verify(sig, pub, hash, 32) == CRYPTO_ERR_VERIFY, failures);
    hash[0] ^= 0x01;
    sig[40] ^= 0x01;
    check("P-256 refuses a changed signature",
          p256_verify(sig, pub, hash, 32) == CRYPTO_ERR_VERIFY, failures);
    sig[40] ^= 0x01;
    pub[63] ^= 0x01;
    check("P-256 refuses a public key that is not on the curve",
          p256_verify(sig, pub, hash, 32) == CRYPTO_ERR_BAD_KEY, failures);
    pub[63] ^= 0x01;

    unhex(d2, 32, ECDH_D2);
    unhex(p2, 64, ECDH_P2);
    int rc = p256_ecdh(shared, priv, p2);
    check_bytes("P-256 ECDH, RFC 6979's key with the test peer", shared, 32, ECDH_SHARED, failures);
    if (rc != CRYPTO_OK) check("P-256 ECDH returned OK", false, failures);
    p256_ecdh(shared, d2, pub);
    check_bytes("P-256 ECDH, the other side", shared, 32, ECDH_SHARED, failures);

    if (random_is_hardware()) {
        uint8_t kpriv[32], kpub[64];
        bool ok = p256_sign(sig, priv, hash, 32) == CRYPTO_OK &&
                  p256_verify(sig, pub, hash, 32) == CRYPTO_OK;
        check("P-256 signs, and its signature verifies", ok, failures);
        uint8_t sig2[64];
        ok = p256_sign(sig2, priv, hash, 32) == CRYPTO_OK && !crypto_equal(sig, sig2, 64);
        check("two signatures of one hash differ (a fresh nonce each)", ok, failures);
        ok = p256_keypair(kpriv, kpub) == CRYPTO_OK &&
             p256_ecdh(shared, kpriv, p2) == CRYPTO_OK;
        uint8_t back[32];
        ok = ok && p256_ecdh(back, d2, kpub) == CRYPTO_OK && crypto_equal(shared, back, 32);
        check("a fresh P-256 key pair agrees with the test peer", ok, failures);
        crypto_wipe_bytes(kpriv, sizeof(kpriv));
    } else {
        check("P-256 signing refuses without hardware entropy",
              p256_sign(sig, priv, hash, 32) == CRYPTO_ERR_NO_ENTROPY, failures);
        check("P-256 key generation refuses without hardware entropy",
              p256_keypair(d2, p2) == CRYPTO_ERR_NO_ENTROPY, failures);
        skip("P-256 signing and key generation");
    }
}

int crypto_selftest(void) {
    int failures = 0;
    cprintf("crypto selftest (entropy: %s):\n", random_source());
    test_sha1(&failures);
    test_hkdf(&failures);
    test_x25519(&failures);
    test_chachapoly(&failures);
    test_p256(&failures);
    if (failures == 0) cprintf("CRYPTO_SELFTEST_OK\n");
    else               cprintf("CRYPTO_SELFTEST_FAIL (%d failed)\n", failures);
    return failures;
}

/* --- timings ------------------------------------------------------------------ */

static void bench_line(const char *what, uint64_t t0, unsigned n) {
    uint64_t us = (time_get_us() - t0) / n;
    cprintf("  %-34s %8lu us\n", what, (unsigned long)us);
}

void crypto_bench(void) {
    /* 256 bytes processed four times per kilobyte: a 1 KB static would cost
     * the P4's full LOWRAM (.bss) a kilobyte for a benchmark. */
    uint8_t buf[256];
    memset(buf, 0x33, sizeof(buf));
    uint8_t key[32], nonce[12], tag[16], out[32], priv[32], pub[64], sig[64], hash[32], p2[64];
    memset(key, 0x11, sizeof(key));
    memset(nonce, 0x22, sizeof(nonce));
    unhex(priv, 32, RFC6979_PRIV);
    unhex(pub, 64, RFC6979_PUB);
    unhex(p2, 64, ECDH_P2);
    unhex(sig, 64, RFC6979_SIG_TEST);
    sha256("test", 4, hash);

    cprintf("crypto timings (per operation):\n");
    uint64_t t0 = time_get_us();
    for (unsigned i = 0; i < 16; i++) {
        sha256_ctx_t c;
        sha256_init(&c);
        for (unsigned q = 0; q < 4; q++) sha256_update(&c, buf, sizeof(buf));
        sha256_final(&c, out);
    }
    bench_line("SHA-256, 1 KB", t0, 16);
    t0 = time_get_us();
    for (unsigned i = 0; i < 16 * 4; i++) chachapoly_seal(buf, tag, key, nonce, NULL, 0, buf, sizeof(buf));
    bench_line("ChaCha20-Poly1305 seal, 4 x 256 B", t0, 16);
    t0 = time_get_us();
    for (unsigned i = 0; i < 2; i++) x25519_shared(out, key, priv);
    bench_line("X25519 shared secret", t0, 2);
    t0 = time_get_us();
    for (unsigned i = 0; i < 2; i++) p256_verify(sig, pub, hash, 32);
    bench_line("P-256 ECDSA verify", t0, 2);
    t0 = time_get_us();
    for (unsigned i = 0; i < 2; i++) p256_ecdh(out, priv, p2);
    bench_line("P-256 ECDH", t0, 2);
    if (random_is_hardware()) {
        t0 = time_get_us();
        for (unsigned i = 0; i < 2; i++) p256_sign(sig, priv, hash, 32);
        bench_line("P-256 ECDSA sign", t0, 2);
    } else {
        cprintf("  %-34s %8s\n", "P-256 ECDSA sign", "skipped");
    }
    crypto_wipe_bytes(buf, sizeof(buf));
}
