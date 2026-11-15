#ifndef LUGALOS_KERNEL_SHA512_H
#define LUGALOS_KERNEL_SHA512_H

#include <stddef.h>
#include <stdint.h>

#define SHA384_DIGEST_LEN 48u
#define SHA512_DIGEST_LEN 64u

typedef struct {
    uint64_t state[8];
    uint64_t total_hi;
    uint64_t total_lo;
    uint8_t  buf[128];
    uint32_t buf_len;
    uint8_t  is_384;
} sha512_ctx_t;

void sha512_init(sha512_ctx_t *ctx);
void sha384_init(sha512_ctx_t *ctx);
void sha512_update(sha512_ctx_t *ctx, const void *data, size_t len);
void sha512_final(sha512_ctx_t *ctx, uint8_t *out);

void sha384(const void *data, uint32_t len, uint8_t out[SHA384_DIGEST_LEN]);

#endif /* LUGALOS_KERNEL_SHA512_H */
