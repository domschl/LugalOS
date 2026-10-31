#ifndef KERNEL_DER_H
#define KERNEL_DER_H

#include <stdbool.h>
#include <stdint.h>

/* The ASN.1 DER that a certificate request and a certificate need, base64 and PEM: enough to build a
 * PKCS#10 request and read an X.509 certificate, nothing general. No allocation, no floating point;
 * target-independent, so tests/host/der_host.c checks it against OpenSSL's output.
 *
 * Writer: der_begin() opens a constructed element, der_end() closes the innermost one and fixes its
 * length (moving the content when the length needs more than one byte). Any overflow or misuse sets
 * `err` and every later call does nothing; check it once at the end. Lengths up to 65535. */

#define DER_SEQUENCE   0x30u
#define DER_SET        0x31u
#define DER_INTEGER    0x02u
#define DER_BIT_STRING 0x03u
#define DER_OCTET_STRING 0x04u
#define DER_NULL       0x05u
#define DER_OID        0x06u
#define DER_UTF8       0x0cu
#define DER_PRINTABLE  0x13u
#define DER_IA5        0x16u
#define DER_UTCTIME    0x17u
#define DER_GENTIME    0x18u
#define DER_CTX(n)      (0xa0u | (n))   /* [n] constructed */
#define DER_CTX_PRIM(n) (0x80u | (n))   /* [n] primitive (implicit) */

#define DER_MAX_DEPTH 10u

typedef struct {
    uint8_t *buf;
    uint32_t cap, len;
    uint32_t open[DER_MAX_DEPTH];   /* offset of each open element's length byte */
    uint32_t depth;
    bool     err;
} der_writer_t;

void der_writer_init(der_writer_t *w, uint8_t *buf, uint32_t cap);
void der_begin(der_writer_t *w, uint8_t tag);
void der_end(der_writer_t *w);
/* A primitive element with `len` content bytes. */
void der_put(der_writer_t *w, uint8_t tag, const void *val, uint32_t len);
/* An INTEGER from an unsigned big-endian number: leading zeros dropped, a 0x00 added when the top
 * bit is set (DER's minimal two's complement). */
void der_put_uint(der_writer_t *w, const uint8_t *be, uint32_t len);
void der_put_small(der_writer_t *w, uint32_t v);
/* Raw bytes, already DER (a nested structure built elsewhere). */
void der_put_raw(der_writer_t *w, const void *der, uint32_t len);

/* Reader: a span of DER, consumed one element at a time. */
typedef struct {
    const uint8_t *p;
    uint32_t len;
} der_span_t;

/* The next element of `in`: its tag and content. Advances `in` past it. False if `in` is empty or the
 * element is malformed (indefinite or over-long length, content running past the span). */
bool der_next(der_span_t *in, uint8_t *tag, der_span_t *val);
/* der_next() that also requires the tag. */
bool der_expect(der_span_t *in, uint8_t tag, der_span_t *val);
/* The whole next element, tag and length included (what a signature covers). */
bool der_next_raw(der_span_t *in, der_span_t *whole);

/* base64 (RFC 4648, with padding). Encode returns the characters written (no NUL), or 0 if `cap` is
 * too small. Decode skips whitespace and returns the bytes written, or -1 on a bad character or a
 * full `out`. */
uint32_t base64_encode(const uint8_t *in, uint32_t n, char *out, uint32_t cap);
int base64_decode(const char *in, uint32_t n, uint8_t *out, uint32_t cap);

/* PEM: "-----BEGIN <label>-----", base64 in 64-character lines, "-----END <label>-----", each line
 * ending in \n. Returns the characters written (no NUL), or 0 if `cap` is too small. */
uint32_t pem_encode(const char *label, const uint8_t *der, uint32_t n, char *out, uint32_t cap);
/* The first <label> block in text[0..n): decodes it into `der` and returns its length, or -1 if there
 * is no complete block or it does not decode. `*end` (may be NULL) gets the offset after its END line. */
int pem_decode(const char *text, uint32_t n, const char *label, uint8_t *der, uint32_t cap, uint32_t *end);

#endif /* KERNEL_DER_H */
