#ifndef LUGALOS_KERNEL_CRYPTO_H
#define LUGALOS_KERNEL_CRYPTO_H

#include <stdint.h>
#include <stdbool.h>

/*
 * The primitives remote administration needs (plan/phase52_remote_admin.md
 * 52.6): signed update images, TLS 1.3, WebSocket handshakes and
 * integrity-protected 9P sessions.
 *
 *   SHA-1               WebSocket's Sec-WebSocket-Accept. Not for security.
 *   HKDF-SHA-256        TLS 1.3's key schedule, 9P session keys (RFC 5869).
 *   X25519              TLS 1.3 key exchange (RFC 7748).
 *   ChaCha20-Poly1305   TLS 1.3's record protection (RFC 8439).
 *   ECDSA / ECDH P-256  certificates and image signatures (FIPS 186-4).
 *
 * SHA-256 and HMAC-SHA-256 stay in kernel/sha256.h. The curve arithmetic and
 * the stream cipher are vendored (third_party/crypto/: Monocypher, p256-m;
 * see doc/about.md). Only kernel/crypto.c calls them, so a library can be
 * replaced, or a hardware accelerator put underneath, without touching a
 * caller.
 *
 * **Formats.** P-256 private keys and ECDH secrets are 32-byte big-endian
 * integers. Public keys are 64 bytes, X then Y, big-endian, without SEC1's
 * 0x04 prefix. Signatures are r then s, 32 bytes each. X25519 keys are
 * RFC 7748's little-endian 32 bytes.
 *
 * **Entropy.** Generating a key or an ECDSA signature needs unpredictable
 * bytes. A predictable signing nonce gives away the private key from one
 * signature. These calls return CRYPTO_ERR_NO_ENTROPY when
 * random_is_hardware() is false (the QEMU targets, a P4 without its entropy
 * source, a C6 with its radio off) and leave their outputs untouched.
 *
 * **Constant time.** Every operation on a secret takes time that depends only
 * on the lengths involved. Compare MACs and tags with crypto_equal(), never
 * memcmp(). See third_party/crypto/<lib>/ORIGIN.md for each library's
 * assumption about the multiplier.
 *
 * **Buffers.** All caller-owned. Nothing here allocates or keeps state between
 * calls, so every function is safe to call from any task. Secret intermediates
 * are wiped before returning.
 */

#define CRYPTO_OK               0
#define CRYPTO_ERR_NO_ENTROPY  -1   /* no hardware entropy: key generation and signing refused */
#define CRYPTO_ERR_BAD_KEY     -2   /* a public key not on the curve, or an invalid private key */
#define CRYPTO_ERR_BAD_INPUT   -3   /* a length out of range */
#define CRYPTO_ERR_VERIFY      -4   /* a signature or tag that does not verify */

/* --- SHA-1 (FIPS 180-4) ----------------------------------------------- */

#define SHA1_DIGEST_LEN 20u

typedef struct {
    uint32_t h[5];
    uint64_t bits;
    uint8_t  block[64];
    uint32_t block_len;
} sha1_ctx_t;

void sha1_init(sha1_ctx_t *ctx);
void sha1_update(sha1_ctx_t *ctx, const void *data, uint32_t len);
void sha1_final(sha1_ctx_t *ctx, uint8_t out[SHA1_DIGEST_LEN]);
void sha1(const void *data, uint32_t len, uint8_t out[SHA1_DIGEST_LEN]);

/* --- HKDF-SHA-256 (RFC 5869) ------------------------------------------- */

#define HKDF_SHA256_PRK_LEN 32u
#define HKDF_SHA256_OUT_MAX (255u * 32u)

/* `salt` may be NULL with salt_len 0: RFC 5869's "a string of HashLen zeros". */
void hkdf_sha256_extract(const void *salt, uint32_t salt_len,
                         const void *ikm, uint32_t ikm_len,
                         uint8_t prk[HKDF_SHA256_PRK_LEN]);
/* CRYPTO_ERR_BAD_INPUT when out_len exceeds HKDF_SHA256_OUT_MAX. */
int hkdf_sha256_expand(const uint8_t prk[HKDF_SHA256_PRK_LEN],
                       const void *info, uint32_t info_len,
                       uint8_t *out, uint32_t out_len);

/* --- X25519 (RFC 7748) --------------------------------------------------- */

#define X25519_KEY_LEN 32u

/* A fresh key pair. CRYPTO_ERR_NO_ENTROPY without hardware entropy. */
int x25519_keypair(uint8_t secret[X25519_KEY_LEN], uint8_t pub[X25519_KEY_LEN]);
void x25519_public(uint8_t pub[X25519_KEY_LEN], const uint8_t secret[X25519_KEY_LEN]);
/* CRYPTO_ERR_BAD_KEY when the result is all zeros (a low-order peer key,
 * which RFC 8446 §7.4.2 says to abort on). `shared` is zeroed then. */
int x25519_shared(uint8_t shared[X25519_KEY_LEN],
                  const uint8_t secret[X25519_KEY_LEN],
                  const uint8_t peer_pub[X25519_KEY_LEN]);

/* --- ChaCha20-Poly1305 AEAD (RFC 8439) ----------------------------------- */

#define CHACHAPOLY_KEY_LEN   32u
#define CHACHAPOLY_NONCE_LEN 12u
#define CHACHAPOLY_TAG_LEN   16u

/* Encrypts `len` bytes of `pt` into `ct` (they may be the same buffer) and
 * writes the tag. `ad` may be NULL when ad_len is 0. */
void chachapoly_seal(uint8_t *ct, uint8_t tag[CHACHAPOLY_TAG_LEN],
                     const uint8_t key[CHACHAPOLY_KEY_LEN],
                     const uint8_t nonce[CHACHAPOLY_NONCE_LEN],
                     const void *ad, uint32_t ad_len,
                     const uint8_t *pt, uint32_t len);
/* Checks the tag before decrypting anything. CRYPTO_OK and `len` bytes of
 * plaintext in `pt`, or CRYPTO_ERR_VERIFY with `pt` untouched. `pt` and `ct`
 * may be the same buffer. */
int chachapoly_open(uint8_t *pt,
                    const uint8_t key[CHACHAPOLY_KEY_LEN],
                    const uint8_t nonce[CHACHAPOLY_NONCE_LEN],
                    const void *ad, uint32_t ad_len,
                    const uint8_t *ct, uint32_t len,
                    const uint8_t tag[CHACHAPOLY_TAG_LEN]);

/* --- P-256 ----------------------------------------------------------------- */

#define P256_PRIV_LEN 32u
#define P256_PUB_LEN  64u
#define P256_SIG_LEN  64u

/* A fresh key pair. CRYPTO_ERR_NO_ENTROPY without hardware entropy. */
int p256_keypair(uint8_t priv[P256_PRIV_LEN], uint8_t pub[P256_PUB_LEN]);
/* Signs a hash (normally SHA-256, 32 bytes). CRYPTO_ERR_NO_ENTROPY without
 * hardware entropy, CRYPTO_ERR_BAD_KEY for an invalid private key. */
int p256_sign(uint8_t sig[P256_SIG_LEN], const uint8_t priv[P256_PRIV_LEN],
              const uint8_t *hash, uint32_t hash_len);
/* CRYPTO_OK when the signature is valid; CRYPTO_ERR_BAD_KEY for a public key
 * not on the curve; CRYPTO_ERR_VERIFY otherwise. Needs no entropy. */
int p256_verify(const uint8_t sig[P256_SIG_LEN], const uint8_t pub[P256_PUB_LEN],
                const uint8_t *hash, uint32_t hash_len);
/* The shared X coordinate. CRYPTO_ERR_BAD_KEY for either key invalid. */
int p256_ecdh(uint8_t shared[32], const uint8_t priv[P256_PRIV_LEN],
              const uint8_t peer_pub[P256_PUB_LEN]);

/* --- helpers ----------------------------------------------------------------- */

/* Constant-time equality, as sha256_verify(). True when equal. */
bool crypto_equal(const void *a, const void *b, uint32_t len);
/* Zeroes `len` bytes in a way the compiler cannot drop as a dead store. */
void crypto_wipe_bytes(void *p, uint32_t len);

/* Known-answer tests for everything above (FIPS 180-4, RFC 5869, RFC 7748,
 * RFC 8439, RFC 6979's P-256 vectors), plus round trips and tamper checks.
 * Key generation and signing are skipped, and said to be, without hardware
 * entropy. Prints one line per case and CRYPTO_SELFTEST_OK or
 * CRYPTO_SELFTEST_FAIL. Returns the number of failures. */
int crypto_selftest(void);

/* `cryptotest bench`: the time of each operation on this chip, in
 * microseconds, for sizing TLS on it. */
void crypto_bench(void);

#endif /* LUGALOS_KERNEL_CRYPTO_H */
