#ifndef LUGALOS_KERNEL_SHA256_H
#define LUGALOS_KERNEL_SHA256_H

#include <stdint.h>
#include <stdbool.h>

/*
 * SHA-256 (FIPS 180-4) and HMAC-SHA-256 (RFC 2104) — N1,
 * plan/phase18_networking_and_auth.md.
 *
 * The first cryptography in this tree, and it exists for exactly one reason:
 * phase 18's `Tauth` gate proves a client knows a pre-shared key without
 * sending it, which is an HMAC over a server-chosen nonce. Nothing else here
 * needs a hash, and this header should not grow into a "crypto library"
 * without a caller asking for it first.
 *
 * Target-independent on purpose, and built on every target including the QEMU
 * ones that will never have a network cable: the fiddly part of an auth
 * exchange is the arithmetic, and arithmetic should not be debugged by
 * flashing a board (the same argument drivers/dcf77_decode.c and
 * drivers/pico_clock_ui.c already make for their own logic).
 *
 * What this is NOT: constant-time. A `memcmp()` on a MAC leaks, through
 * timing, how many leading bytes matched — which over a network is a
 * practical forgery oracle, not a theoretical one. Comparisons of anything
 * secret go through sha256_verify() below, never memcmp().
 */

#define SHA256_DIGEST_LEN 32u
#define SHA256_BLOCK_LEN  64u

typedef struct {
    uint32_t h[8];
    uint64_t bits;               /* message length so far, in bits */
    uint8_t  block[SHA256_BLOCK_LEN];
    uint32_t block_len;          /* bytes buffered in `block` */
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, uint32_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_LEN]);

/* One-shot convenience. */
void sha256(const void *data, uint32_t len, uint8_t out[SHA256_DIGEST_LEN]);

/* HMAC-SHA-256. `key` of any length: longer than the 64-byte block is hashed
 * first, shorter is zero-padded, exactly as RFC 2104 says. */
void hmac_sha256(const void *key, uint32_t key_len,
                 const void *msg, uint32_t msg_len,
                 uint8_t out[SHA256_DIGEST_LEN]);

/* Constant-time equality over `len` bytes: the time taken depends on `len`
 * and on nothing else. Returns true if equal.
 *
 * Use this and not memcmp() for every comparison where one side is secret or
 * attacker-supplied. memcmp() returns as soon as two bytes differ, so the
 * time it takes says how long the common prefix was, and an attacker who can
 * measure that recovers a MAC one byte at a time instead of guessing 2^256.
 */
bool sha256_verify(const void *a, const void *b, uint32_t len);

/* Known-answer tests: FIPS 180-4's SHA-256 examples and RFC 4231's
 * HMAC-SHA-256 test cases 1-4, 6 and 7 (the two that exercise a key longer
 * than the block). Prints one line per case and a machine-readable verdict.
 * Returns the number of failures, 0 if everything passed. */
int sha256_selftest(void);

#endif /* LUGALOS_KERNEL_SHA256_H */
