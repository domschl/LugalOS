#ifndef LUGALOS_KERNEL_RSA_H
#define LUGALOS_KERNEL_RSA_H

#include <stdbool.h>
#include <stdint.h>

#define RSA_MAX_BYTES 512u /* up to 4096-bit RSA */

#define RSA_HASH_SHA256 1
#define RSA_HASH_SHA384 2
#define RSA_HASH_SHA512 3

/* Verify an RSASSA-PKCS1-v1_5 signature (RFC 8017 §8.2.2). Returns true if valid. */
bool rsa_verify_pkcs1_v15(const uint8_t *n, uint32_t n_len,
                          const uint8_t *e, uint32_t e_len,
                          int hash_alg, const uint8_t *digest, uint32_t digest_len,
                          const uint8_t *sig, uint32_t sig_len);

/* Verify an RSASSA-PSS signature (RFC 8017 §8.1.2) with MGF1(hash_alg) and sLen == digest_len.
 * Returns true if valid. */
bool rsa_verify_pss(const uint8_t *n, uint32_t n_len,
                    const uint8_t *e, uint32_t e_len,
                    int hash_alg, const uint8_t *digest, uint32_t digest_len,
                    const uint8_t *sig, uint32_t sig_len);

#endif /* LUGALOS_KERNEL_RSA_H */
