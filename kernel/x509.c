/* PKCS#10 requests out, X.509 certificates and chains in, on top of kernel/der.c.
 * Target-independent (tests/host/der_host.c). */

#include "kernel/tls_cert.h"
#include "kernel/der.h"
#include "kernel/crypto.h"
#include "kernel/sha256.h"
#include "kernel/sha512.h"
#include "kernel/rsa.h"
#include <string.h>

static const uint8_t OID_EC_PUBKEY[]     = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };             /* 1.2.840.10045.2.1 */
static const uint8_t OID_P256[]          = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };       /* 1.2.840.10045.3.1.7 */
static const uint8_t OID_ECDSA_SHA256[]  = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };       /* 1.2.840.10045.4.3.2 */
static const uint8_t OID_RSA_ENCR[]      = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 }; /* 1.2.840.113549.1.1.1 */
static const uint8_t OID_RSA_PSS[]       = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0a }; /* 1.2.840.113549.1.1.10 */
static const uint8_t OID_RSA_SHA256[]    = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b }; /* 1.2.840.113549.1.1.11 */
static const uint8_t OID_RSA_SHA384[]    = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c }; /* 1.2.840.113549.1.1.12 */
static const uint8_t OID_RSA_SHA512[]    = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d }; /* 1.2.840.113549.1.1.13 */
static const uint8_t OID_SHA256[]        = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01 }; /* 2.16.840.1.101.3.4.2.1 */
static const uint8_t OID_SHA384[]        = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02 }; /* 2.16.840.1.101.3.4.2.2 */
static const uint8_t OID_SHA512[]        = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03 }; /* 2.16.840.1.101.3.4.2.3 */
static const uint8_t OID_CN[]            = { 0x55, 0x04, 0x03 };                                     /* 2.5.4.3 */
static const uint8_t OID_EXT_REQ[]       = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x0e }; /* 1.2.840.113549.1.9.14 */
static const uint8_t OID_SAN[]           = { 0x55, 0x1d, 0x11 };                                     /* 2.5.29.17 */

static bool oid_is(der_span_t oid, const uint8_t *want, uint32_t n) {
    return oid.len == n && memcmp(oid.p, want, n) == 0;
}

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

    uint8_t hash[SHA256_DIGEST_LEN], sig[P256_SIG_LEN];
    sha256(&out[info_at], w.len - info_at, hash);
    int rc = p256_sign(sig, priv, hash, sizeof hash);
    if (rc == CRYPTO_ERR_NO_ENTROPY) return TLS_ERR_ENTROPY;
    if (rc != CRYPTO_OK) return TLS_ERR_NO_KEY;

      der_begin(&w, DER_SEQUENCE);
        der_put(&w, DER_OID, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256);
      der_end(&w);
      uint8_t sv[1 + 2 + 2 * (2 + 33)];
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

static bool parse_spki(der_span_t spki, tls_cert_info_t *out) {
    der_span_t alg, oid, bits;
    uint8_t tag;
    if (!der_expect(&spki, DER_SEQUENCE, &alg) || !der_expect(&alg, DER_OID, &oid)) return false;
    if (oid_is(oid, OID_EC_PUBKEY, sizeof OID_EC_PUBKEY)) {
        if (!der_expect(&alg, DER_OID, &oid) || !oid_is(oid, OID_P256, sizeof OID_P256)) return false;
        if (!der_next(&spki, &tag, &bits) || tag != DER_BIT_STRING || bits.len != 66 ||
            bits.p[0] != 0 || bits.p[1] != 0x04) return false;
        out->key_type = TLS_KEY_P256;
        memcpy(out->pub, bits.p + 2, 64);
        return true;
    }
    if (oid_is(oid, OID_RSA_ENCR, sizeof OID_RSA_ENCR)) {
        if (!der_next(&spki, &tag, &bits) || tag != DER_BIT_STRING || bits.len < 2 || bits.p[0] != 0) return false;
        der_span_t inner = { bits.p + 1, bits.len - 1 }, rsa_seq, n, e;
        if (!der_expect(&inner, DER_SEQUENCE, &rsa_seq) ||
            !der_expect(&rsa_seq, DER_INTEGER, &n) ||
            !der_expect(&rsa_seq, DER_INTEGER, &e)) return false;
        while (n.len > 0 && n.p[0] == 0) { n.p++; n.len--; }
        while (e.len > 0 && e.p[0] == 0) { e.p++; e.len--; }
        if (n.len == 0 || n.len > sizeof(out->rsa_n) || e.len == 0 || e.len > sizeof(out->rsa_e)) return false;
        out->key_type = TLS_KEY_RSA;
        out->rsa_n_len = (uint16_t)n.len;
        out->rsa_e_len = (uint8_t)e.len;
        memcpy(out->rsa_n, n.p, n.len);
        memcpy(out->rsa_e, e.p, e.len);
        return true;
    }
    return false;
}

int tls_cert_parse(const uint8_t *der, uint32_t len, tls_cert_info_t *out) {
    memset(out, 0, sizeof *out);
    der_span_t in = { der, len }, cert, tbs, x;
    if (!der_expect(&in, DER_SEQUENCE, &cert) || in.len != 0) return TLS_ERR_CERT;
    if (!der_expect(&cert, DER_SEQUENCE, &tbs)) return TLS_ERR_CERT;
    (void)der_expect(&tbs, DER_CTX(0), &x);                          /* version, when present */
    if (!der_expect(&tbs, DER_INTEGER, &x)) return TLS_ERR_CERT;     /* serial */
    if (!der_expect(&tbs, DER_SEQUENCE, &x)) return TLS_ERR_CERT;    /* signature algorithm */
    der_span_t issuer, validity, subject, spki;
    if (!der_expect(&tbs, DER_SEQUENCE, &issuer)) return TLS_ERR_CERT;
    if (!der_expect(&tbs, DER_SEQUENCE, &validity)) return TLS_ERR_CERT;
    if (!read_time(&validity, out->not_before, sizeof out->not_before)) return TLS_ERR_CERT;
    if (!read_time(&validity, out->not_after, sizeof out->not_after)) return TLS_ERR_CERT;
    if (!der_expect(&tbs, DER_SEQUENCE, &subject)) return TLS_ERR_CERT;
    if (!der_expect(&tbs, DER_SEQUENCE, &spki)) return TLS_ERR_CERT;
    if (!parse_spki(spki, out)) return TLS_ERR_CERT;
    name_cn(issuer, out->issuer_cn, sizeof out->issuer_cn);
    name_cn(subject, out->subject_cn, sizeof out->subject_cn);
    return TLS_OK;
}

/* An INTEGER's value as a 32-byte big-endian number (leading zeros dropped, then left-padded). */
static bool int32(der_span_t v, uint8_t out[32]) {
    while (v.len > 0 && v.p[0] == 0) { v.p++; v.len--; }
    if (v.len > 32) return false;
    memset(out, 0, 32);
    memcpy(out + 32 - v.len, v.p, v.len);
    return true;
}

static bool dotted_ipv4(const char *s, uint8_t ip[4]) {
    for (int i = 0; i < 4; i++) {
        uint32_t v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10u + (uint32_t)(*s++ - '0'); if (++digits > 3) return false; }
        if (digits == 0 || v > 255) return false;
        ip[i] = (uint8_t)v;
        if (i < 3 && *s++ != '.') return false;
    }
    return *s == 0;
}

static bool ci_eq(const uint8_t *a, const char *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint8_t x = a[i], y = (uint8_t)b[i];
        if (x >= 'A' && x <= 'Z') x = (uint8_t)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (uint8_t)(y + 32);
        if (x != y) return false;
    }
    return true;
}

static bool name_eq(const uint8_t *a, uint32_t n, const char *host) {
    uint32_t hlen = (uint32_t)strlen(host);
    if (n >= 3 && a[0] == '*' && a[1] == '.') {
        /* Single-label wildcard: *.example.com matches foo.example.com */
        const char *dot = strchr(host, '.');
        if (!dot || dot == host) return false;
        uint32_t suffix_len = hlen - (uint32_t)(dot - host);
        if (suffix_len != n - 1u) return false;
        return ci_eq(a + 1, dot, suffix_len);
    }
    if (hlen != n) return false;
    return ci_eq(a, host, n);
}

/* 1: the subjectAltName names `host`; 0: it names others; -1: the certificate has none. */
static int san_names(der_span_t tbs, const char *host) {
    uint8_t ip[4];
    bool is_ip = dotted_ipv4(host, ip);
    der_span_t x;
    uint8_t tag;
    while (der_next(&tbs, &tag, &x)) {
        if (tag != DER_CTX(3)) continue;                         /* extensions, after the key */
        der_span_t exts, ext;
        if (!der_expect(&x, DER_SEQUENCE, &exts)) return -1;
        while (der_expect(&exts, DER_SEQUENCE, &ext)) {
            der_span_t oid, val, gn;
            if (!der_expect(&ext, DER_OID, &oid) || !oid_is(oid, OID_SAN, sizeof OID_SAN)) continue;
            if (!der_next(&ext, &tag, &val)) return 0;
            if (tag != DER_OCTET_STRING && !der_next(&ext, &tag, &val)) return 0;   /* past `critical` */
            if (!der_expect(&val, DER_SEQUENCE, &gn)) return 0;
            der_span_t n;
            while (der_next(&gn, &tag, &n)) {
                if (is_ip && tag == DER_CTX_PRIM(7) && n.len == 4 && memcmp(n.p, ip, 4) == 0) return 1;
                if (!is_ip && tag == DER_CTX_PRIM(2) && name_eq(n.p, n.len, host)) return 1;
            }
            return 0;
        }
    }
    return -1;
}

static bool verify_tbs_sig(const uint8_t *tbs_p, uint32_t tbs_len,
                           der_span_t alg, der_span_t bits,
                           const tls_cert_info_t *issuer) {
    der_span_t oid;
    if (!der_expect(&alg, DER_OID, &oid)) return false;
    if (bits.len < 2 || bits.p[0] != 0) return false;
    const uint8_t *sig_bytes = bits.p + 1;
    uint32_t sig_len = bits.len - 1u;

    if (issuer->key_type == TLS_KEY_P256 && oid_is(oid, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256)) {
        der_span_t inner = { sig_bytes, sig_len }, sv, r, s;
        uint8_t sig[P256_SIG_LEN], hash[SHA256_DIGEST_LEN];
        if (!der_expect(&inner, DER_SEQUENCE, &sv) || !der_expect(&sv, DER_INTEGER, &r) ||
            !der_expect(&sv, DER_INTEGER, &s) || !int32(r, sig) || !int32(s, sig + 32)) return false;
        sha256(tbs_p, tbs_len, hash);
        return p256_verify(sig, issuer->pub, hash, sizeof hash) == CRYPTO_OK;
    }

    if (issuer->key_type == TLS_KEY_RSA) {
        int hash_alg = 0;
        bool is_pss = false;
        if (oid_is(oid, OID_RSA_SHA256, sizeof OID_RSA_SHA256)) {
            hash_alg = RSA_HASH_SHA256;
        } else if (oid_is(oid, OID_RSA_SHA384, sizeof OID_RSA_SHA384)) {
            hash_alg = RSA_HASH_SHA384;
        } else if (oid_is(oid, OID_RSA_SHA512, sizeof OID_RSA_SHA512)) {
            hash_alg = RSA_HASH_SHA512;
        } else if (oid_is(oid, OID_RSA_PSS, sizeof OID_RSA_PSS)) {
            is_pss = true;
            hash_alg = RSA_HASH_SHA256;
            der_span_t params, h_ctx, h_alg, h_oid;
            if (der_expect(&alg, DER_SEQUENCE, &params) &&
                der_expect(&params, DER_CTX(0), &h_ctx) &&
                der_expect(&h_ctx, DER_SEQUENCE, &h_alg) &&
                der_expect(&h_alg, DER_OID, &h_oid)) {
                if (oid_is(h_oid, OID_SHA384, sizeof OID_SHA384)) hash_alg = RSA_HASH_SHA384;
                else if (oid_is(h_oid, OID_SHA512, sizeof OID_SHA512)) hash_alg = RSA_HASH_SHA512;
                else if (!oid_is(h_oid, OID_SHA256, sizeof OID_SHA256)) return false;
            }
        } else {
            return false;
        }

        uint8_t digest[64];
        uint32_t dlen = 32u;
        if (hash_alg == RSA_HASH_SHA256) {
            sha256(tbs_p, tbs_len, digest);
            dlen = 32u;
        } else if (hash_alg == RSA_HASH_SHA384) {
            sha384(tbs_p, tbs_len, digest);
            dlen = 48u;
        } else {
            sha512(tbs_p, tbs_len, digest);
            dlen = 64u;
        }

        if (is_pss) {
            return rsa_verify_pss(issuer->rsa_n, issuer->rsa_n_len,
                                  issuer->rsa_e, issuer->rsa_e_len,
                                  hash_alg, digest, dlen, sig_bytes, sig_len);
        }
        return rsa_verify_pkcs1_v15(issuer->rsa_n, issuer->rsa_n_len,
                                    issuer->rsa_e, issuer->rsa_e_len,
                                    hash_alg, digest, dlen, sig_bytes, sig_len);
    }

    return false;
}

typedef struct {
    der_span_t tbs_whole;
    der_span_t issuer_raw;
    der_span_t subject_raw;
    der_span_t alg;
    der_span_t bits;
} x509_parts_t;

static bool extract_parts(const uint8_t *der, uint32_t len, x509_parts_t *p) {
    der_span_t in = { der, len }, cert, tbs, x;
    uint8_t tag;
    if (!der_expect(&in, DER_SEQUENCE, &cert) || in.len != 0) return false;
    if (!der_next_raw(&cert, &p->tbs_whole)) return false;
    if (!der_expect(&cert, DER_SEQUENCE, &p->alg)) return false;
    if (!der_next(&cert, &tag, &p->bits) || tag != DER_BIT_STRING) return false;

    der_span_t whole = p->tbs_whole;
    if (!der_expect(&whole, DER_SEQUENCE, &tbs)) return false;
    (void)der_expect(&tbs, DER_CTX(0), &x);
    if (!der_expect(&tbs, DER_INTEGER, &x)) return false;
    if (!der_expect(&tbs, DER_SEQUENCE, &x)) return false;
    if (!der_next_raw(&tbs, &p->issuer_raw)) return false;
    if (!der_expect(&tbs, DER_SEQUENCE, &x)) return false; /* validity */
    if (!der_next_raw(&tbs, &p->subject_raw)) return false;
    return true;
}

static uint16_t get_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool tls_trust_store_init(tls_trust_store_t *store, const uint8_t *data, uint32_t len) {
    if (!store || !data || len < 8u) return false;
    if (memcmp(data, "LCRT", 4) != 0) return false;
    uint16_t ver = get_le16(data + 4);
    uint16_t cnt = get_le16(data + 6);
    if (ver != 1u || cnt == 0u) return false;
    uint32_t off = 8u;
    for (uint32_t i = 0; i < cnt; i++) {
        if (off + 36u > len) return false;
        uint8_t kt = data[off + 32u];
        uint8_t elen = data[off + 33u];
        uint16_t klen = get_le16(data + off + 34u);
        if (kt == TLS_KEY_P256 && (klen != 64u || elen != 0u)) return false;
        if (kt == TLS_KEY_RSA && (klen == 0u || klen > RSA_MAX_BYTES || elen == 0u || elen > 8u)) return false;
        if (kt != TLS_KEY_P256 && kt != TLS_KEY_RSA) return false;
        off += 36u + (uint32_t)klen + (uint32_t)elen;
        if (off > len) return false;
    }
    store->data = data;
    store->len = len;
    store->count = cnt;
    return true;
}

static bool verify_against_store(const x509_parts_t *parts, const tls_trust_store_t *store) {
    uint8_t issuer_hash[SHA256_DIGEST_LEN];
    sha256(parts->issuer_raw.p, parts->issuer_raw.len, issuer_hash);

    uint32_t off = 8u;
    for (uint32_t i = 0; i < store->count; i++) {
        const uint8_t *ent = store->data + off;
        uint8_t kt = ent[32];
        uint8_t elen = ent[33];
        uint16_t klen = get_le16(ent + 34);
        if (memcmp(ent, issuer_hash, 32) == 0) {
            tls_cert_info_t root;
            memset(&root, 0, sizeof(root));
            root.key_type = kt;
            if (kt == TLS_KEY_P256) {
                memcpy(root.pub, ent + 36, 64);
            } else {
                root.rsa_n_len = klen;
                root.rsa_e_len = elen;
                memcpy(root.rsa_n, ent + 36, klen);
                memcpy(root.rsa_e, ent + 36 + klen, elen);
            }
            if (verify_tbs_sig(parts->tbs_whole.p, parts->tbs_whole.len,
                               parts->alg, parts->bits, &root)) {
                return true;
            }
        }
        off += 36u + (uint32_t)klen + (uint32_t)elen;
    }
    return false;
}

int tls_cert_check_chain(const uint8_t *const *ders, const uint32_t *lens, uint32_t count,
                         const uint8_t ca_pub[64], const tls_trust_store_t *store,
                         const char *host, tls_cert_info_t *out) {
    if (!ders || !lens || count == 0 || !out) return TLS_ERR_CERT;
    if (tls_cert_parse(ders[0], lens[0], out) != TLS_OK) return TLS_ERR_CERT;

    x509_parts_t cur_parts;
    if (!extract_parts(ders[0], lens[0], &cur_parts)) return TLS_ERR_CERT;

    der_span_t whole = cur_parts.tbs_whole, tbs;
    if (!der_expect(&whole, DER_SEQUENCE, &tbs)) return TLS_ERR_CERT;
    int named = san_names(tbs, host);
    if (named < 0) named = name_eq((const uint8_t *)out->subject_cn, (uint32_t)strlen(out->subject_cn), host);
    if (!named) return TLS_ERR_WRONG_KEY;

    if (ca_pub) {
        tls_cert_info_t ca;
        memset(&ca, 0, sizeof(ca));
        ca.key_type = TLS_KEY_P256;
        memcpy(ca.pub, ca_pub, 64);
        return verify_tbs_sig(cur_parts.tbs_whole.p, cur_parts.tbs_whole.len,
                              cur_parts.alg, cur_parts.bits, &ca) ? TLS_OK : TLS_ERR_CERT;
    }

    if (!store) return TLS_ERR_CERT;

    uint32_t cur = 0;
    uint32_t visited = 1u; /* bitmask for up to 8 certs */
    for (uint32_t depth = 0; depth < count && depth < 8u; depth++) {
        if (verify_against_store(&cur_parts, store)) {
            return TLS_OK;
        }
        bool chained = false;
        for (uint32_t j = 1; j < count && j < 8u; j++) {
            if (visited & (1u << j)) continue;
            x509_parts_t parent_parts;
            if (!extract_parts(ders[j], lens[j], &parent_parts)) continue;
            if (parent_parts.subject_raw.len != cur_parts.issuer_raw.len ||
                memcmp(parent_parts.subject_raw.p, cur_parts.issuer_raw.p, cur_parts.issuer_raw.len) != 0) {
                continue;
            }
            tls_cert_info_t parent_info;
            if (tls_cert_parse(ders[j], lens[j], &parent_info) != TLS_OK) continue;
            if (!verify_tbs_sig(cur_parts.tbs_whole.p, cur_parts.tbs_whole.len,
                                cur_parts.alg, cur_parts.bits, &parent_info)) {
                continue;
            }
            visited |= (1u << j);
            cur = j;
            cur_parts = parent_parts;
            chained = true;
            break;
        }
        if (!chained) break;
    }
    (void)cur;
    return TLS_ERR_CERT;
}

int tls_cert_check(const uint8_t *der, uint32_t len, const uint8_t ca_pub[64], const char *host,
                   tls_cert_info_t *out) {
    return tls_cert_check_chain(&der, &len, 1u, ca_pub, NULL, host, out);
}
