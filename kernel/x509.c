/* PKCS#10 requests out, X.509 certificates in: the two shapes kernel/tls_cert.c needs, on top of
 * kernel/der.c. Target-independent (tests/host/der_host.c). */

#include "kernel/tls_cert.h"
#include "kernel/der.h"
#include "kernel/crypto.h"
#include "kernel/sha256.h"
#include <string.h>

static const uint8_t OID_EC_PUBKEY[]  = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };             /* 1.2.840.10045.2.1 */
static const uint8_t OID_P256[]       = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };       /* 1.2.840.10045.3.1.7 */
static const uint8_t OID_ECDSA_SHA256[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };     /* 1.2.840.10045.4.3.2 */
static const uint8_t OID_CN[]         = { 0x55, 0x04, 0x03 };                                     /* 2.5.4.3 */
static const uint8_t OID_EXT_REQ[]    = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x0e }; /* 1.2.840.113549.1.9.14 */
static const uint8_t OID_SAN[]        = { 0x55, 0x1d, 0x11 };                                     /* 2.5.29.17 */

static void put_spki(der_writer_t *w, const uint8_t pub[64]) {
    der_begin(w, DER_SEQUENCE);
      der_begin(w, DER_SEQUENCE);
        der_put(w, DER_OID, OID_EC_PUBKEY, sizeof OID_EC_PUBKEY);
        der_put(w, DER_OID, OID_P256, sizeof OID_P256);
      der_end(w);
      uint8_t bits[2 + 64];
      bits[0] = 0;                               /* no unused bits */
      bits[1] = 0x04;                            /* uncompressed point */
      memcpy(bits + 2, pub, 64);
      der_put(w, DER_BIT_STRING, bits, sizeof bits);
    der_end(w);
}

int tls_csr_build(const uint8_t priv[32], const uint8_t pub[64], const char *cn, const uint8_t ip[4],
                  uint8_t *out, uint32_t cap) {
    der_writer_t w;
    der_writer_init(&w, out, cap);
    uint32_t cn_len = (uint32_t)strlen(cn);

    der_begin(&w, DER_SEQUENCE);                 /* CertificationRequest */
    uint32_t info_at = w.len;
      der_begin(&w, DER_SEQUENCE);               /* CertificationRequestInfo */
        der_put_small(&w, 0);                    /* version v1 */
        der_begin(&w, DER_SEQUENCE);             /* subject: CN=<cn> */
          der_begin(&w, DER_SET);
            der_begin(&w, DER_SEQUENCE);
              der_put(&w, DER_OID, OID_CN, sizeof OID_CN);
              der_put(&w, DER_UTF8, cn, cn_len);
            der_end(&w);
          der_end(&w);
        der_end(&w);
        put_spki(&w, pub);
        der_begin(&w, DER_CTX(0));               /* attributes */
          der_begin(&w, DER_SEQUENCE);
            der_put(&w, DER_OID, OID_EXT_REQ, sizeof OID_EXT_REQ);
            der_begin(&w, DER_SET);
              der_begin(&w, DER_SEQUENCE);       /* Extensions */
                der_begin(&w, DER_SEQUENCE);     /* subjectAltName */
                  der_put(&w, DER_OID, OID_SAN, sizeof OID_SAN);
                  der_begin(&w, DER_OCTET_STRING);
                    der_begin(&w, DER_SEQUENCE);
                      der_put(&w, DER_CTX_PRIM(2), cn, cn_len);          /* dNSName */
                      if (ip) der_put(&w, DER_CTX_PRIM(7), ip, 4);       /* iPAddress */
                    der_end(&w);
                  der_end(&w);
                der_end(&w);
              der_end(&w);
            der_end(&w);
          der_end(&w);
        der_end(&w);
      der_end(&w);
    if (w.err) return TLS_ERR_MEMORY;

    /* The signature covers the info exactly as encoded: hash it before anything after it is written
     * (der_end() of the outer sequence will move it, not change it). */
    uint8_t hash[SHA256_DIGEST_LEN], sig[P256_SIG_LEN];
    sha256(&out[info_at], w.len - info_at, hash);
    int rc = p256_sign(sig, priv, hash, sizeof hash);
    if (rc == CRYPTO_ERR_NO_ENTROPY) return TLS_ERR_ENTROPY;
    if (rc != CRYPTO_OK) return TLS_ERR_NO_KEY;

      der_begin(&w, DER_SEQUENCE);
        der_put(&w, DER_OID, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256);
      der_end(&w);
      /* BIT STRING { 0 unused bits, ECDSA-Sig-Value ::= SEQUENCE { r INTEGER, s INTEGER } } */
      uint8_t sv[1 + 2 + 2 * (2 + 33)];         /* unused-bits byte, SEQUENCE, two INTEGERs of up to 33 bytes */
      der_writer_t s;
      der_writer_init(&s, sv + 1, sizeof sv - 1);
      der_begin(&s, DER_SEQUENCE);
        der_put_uint(&s, sig, 32);
        der_put_uint(&s, sig + 32, 32);
      der_end(&s);
      if (s.err) return TLS_ERR_MEMORY;
      sv[0] = 0;
      der_put(&w, DER_BIT_STRING, sv, s.len + 1);
    der_end(&w);
    crypto_wipe_bytes(sig, sizeof sig);
    return w.err ? TLS_ERR_MEMORY : (int)w.len;
}

/* The first CN in a Name, as text (truncated to fit). Empty if there is none. */
static void name_cn(der_span_t name, char *out, uint32_t cap) {
    out[0] = 0;
    der_span_t rdn;
    while (der_expect(&name, DER_SET, &rdn)) {
        der_span_t atv;
        while (der_expect(&rdn, DER_SEQUENCE, &atv)) {
            der_span_t oid, val;
            uint8_t tag;
            if (!der_expect(&atv, DER_OID, &oid) || !der_next(&atv, &tag, &val)) continue;
            if (oid.len == sizeof OID_CN && memcmp(oid.p, OID_CN, sizeof OID_CN) == 0) {
                uint32_t n = val.len < cap - 1 ? val.len : cap - 1;
                memcpy(out, val.p, n);
                out[n] = 0;
                return;
            }
        }
    }
}

static bool read_time(der_span_t *in, char *out, uint32_t cap) {
    der_span_t t;
    uint8_t tag;
    if (!der_next(in, &tag, &t) || (tag != DER_UTCTIME && tag != DER_GENTIME) || t.len >= cap) return false;
    memcpy(out, t.p, t.len);
    out[t.len] = 0;
    return true;
}

int tls_cert_parse(const uint8_t *der, uint32_t len, tls_cert_info_t *out) {
    memset(out, 0, sizeof *out);
    der_span_t in = { der, len }, cert, tbs, x;
    uint8_t tag;
    if (!der_expect(&in, DER_SEQUENCE, &cert) || in.len != 0) return TLS_ERR_CERT;
    if (!der_expect(&cert, DER_SEQUENCE, &tbs)) return TLS_ERR_CERT;
    (void)der_expect(&tbs, DER_CTX(0), &x);                          /* version, when present */
    if (!der_expect(&tbs, DER_INTEGER, &x)) return TLS_ERR_CERT;     /* serial */
    if (!der_expect(&tbs, DER_SEQUENCE, &x)) return TLS_ERR_CERT;    /* signature algorithm */
    der_span_t issuer, validity, subject, spki, alg, oid, bits;
    if (!der_expect(&tbs, DER_SEQUENCE, &issuer)) return TLS_ERR_CERT;
    if (!der_expect(&tbs, DER_SEQUENCE, &validity)) return TLS_ERR_CERT;
    if (!read_time(&validity, out->not_before, sizeof out->not_before)) return TLS_ERR_CERT;
    if (!read_time(&validity, out->not_after, sizeof out->not_after)) return TLS_ERR_CERT;
    if (!der_expect(&tbs, DER_SEQUENCE, &subject)) return TLS_ERR_CERT;
    if (!der_expect(&tbs, DER_SEQUENCE, &spki)) return TLS_ERR_CERT;
    if (!der_expect(&spki, DER_SEQUENCE, &alg)) return TLS_ERR_CERT;
    if (!der_expect(&alg, DER_OID, &oid) || oid.len != sizeof OID_EC_PUBKEY ||
        memcmp(oid.p, OID_EC_PUBKEY, sizeof OID_EC_PUBKEY) != 0) return TLS_ERR_CERT;
    if (!der_expect(&alg, DER_OID, &oid) || oid.len != sizeof OID_P256 ||
        memcmp(oid.p, OID_P256, sizeof OID_P256) != 0) return TLS_ERR_CERT;
    if (!der_next(&spki, &tag, &bits) || tag != DER_BIT_STRING || bits.len != 66 ||
        bits.p[0] != 0 || bits.p[1] != 0x04) return TLS_ERR_CERT;
    memcpy(out->pub, bits.p + 2, 64);
    name_cn(issuer, out->issuer_cn, sizeof out->issuer_cn);
    name_cn(subject, out->subject_cn, sizeof out->subject_cn);
    return TLS_OK;
}
