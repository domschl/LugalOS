/* Host test of kernel/der.c and kernel/x509.c: base64 against RFC 4648's vectors, the DER writer's
 * length forms and INTEGER encoding read back by the reader, a certificate OpenSSL made parsed for
 * its names, dates and key, a request built here whose signature verifies over the bytes it signs,
 * and every single-byte change and random input fed to the parsers (which must refuse or parse, and
 * never read outside their input: ASan/UBSan and valgrind watch). */
#include "kernel/der.h"
#include "kernel/tls_cert.h"
#include "kernel/crypto.h"
#include "kernel/sha256.h"
#include "shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

bool random_is_hardware(void) { return true; }
const char *random_source(void) { return "host /dev/urandom"; }
void random_bytes(void *buf, uint32_t len) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(buf, 1, len, f) != len) { fprintf(stderr, "no /dev/urandom\n"); exit(2); }
    fclose(f);
}

static uint64_t g_state;
static uint64_t next(void) {
    g_state ^= g_state >> 12; g_state ^= g_state << 25; g_state ^= g_state >> 27;
    return g_state * 0x2545F4914F6CDD1DULL;
}

/* openssl: CN=hosttest.local, issued by CN=Host Test CA, 2026-10-31 .. 2029-02-02, SAN hosttest.local. */
static const char CERT_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBfTCCASOgAwIBAgICEjQwCgYIKoZIzj0EAwIwFzEVMBMGA1UEAwwMSG9zdCBU\n"
    "ZXN0IENBMB4XDTI2MTAzMTEzMzM0N1oXDTI5MDIwMjEzMzM0N1owGTEXMBUGA1UE\n"
    "AwwOaG9zdHRlc3QubG9jYWwwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAAT6xxdU\n"
    "jSBCxNvkUdY1jto6YcDl2nU6yhx/EY4IwSP8QDVznDm8FBpLffWf6NE8dgQ3BKwf\n"
    "RLz6PciKI08pTBLco10wWzAZBgNVHREEEjAQgg5ob3N0dGVzdC5sb2NhbDAdBgNV\n"
    "HQ4EFgQUO9coNof9QUPHeh5HF7YbJMdJa+AwHwYDVR0jBBgwFoAUsBDecITvQq/g\n"
    "hyfwJj/Xi6ylSPcwCgYIKoZIzj0EAwIDSAAwRQIhANHg7+0+0i0bac3LHKrNEaoN\n"
    "TfqlAWhOOUJuHJnxBUCWAiBR/aOKslnGg5jr3ZwIsIZCqmlWIQCiESC/VeNtvkmn\n"
    "EQ==\n"
    "-----END CERTIFICATE-----\n";
static const char CERT_PUB_HEX[] =
    "fac717548d2042c4dbe451d6358eda3a61c0e5da753aca1c7f118e08c123fc40"
    "35739c39bc141a4b7df59fe8d13c76043704ac1f44bcfa3dc88a234f294c12dc";

static void unhex(const char *h, uint8_t *out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); out[i] = (uint8_t)v; }
}

static void test_base64(void) {
    static const char *in[] = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    static const char *out[] = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy" };
    for (int i = 0; i < 7; i++) {
        char e[16] = {0};
        uint8_t d[16];
        uint32_t n = base64_encode((const uint8_t *)in[i], (uint32_t)strlen(in[i]), e, sizeof e);
        CHECK(n == strlen(out[i]) && memcmp(e, out[i], n) == 0, "base64(%s) = %.*s", in[i], (int)n, e);
        int k = base64_decode(out[i], (uint32_t)strlen(out[i]), d, sizeof d);
        CHECK(k == (int)strlen(in[i]) && memcmp(d, in[i], (size_t)k) == 0, "unbase64(%s)", out[i]);
    }
    uint8_t d[4];
    CHECK(base64_decode("Zm9v!", 5, d, sizeof d) < 0, "a bad character is refused");
    CHECK(base64_decode("Zm9vYmFy", 8, d, 4) < 0, "an output that does not fit is refused");
    char small[3];
    CHECK(base64_encode((const uint8_t *)"foo", 3, small, sizeof small) == 0, "an encode that does not fit");
}

static void test_der_lengths(void) {
    static const uint32_t sizes[] = { 0, 1, 127, 128, 255, 256, 300, 1000 };
    uint8_t buf[2600], payload[1000];
    for (uint32_t i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)i;
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        der_writer_t w;
        der_writer_init(&w, buf, sizeof buf);
        der_begin(&w, DER_SEQUENCE);
          der_begin(&w, DER_SET);
            der_put(&w, DER_OCTET_STRING, payload, sizes[i]);
          der_end(&w);
          der_put_small(&w, sizes[i]);
        der_end(&w);
        CHECK(!w.err, "writer error at %u", sizes[i]);
        der_span_t in = { buf, w.len }, seq, set, os, num;
        bool ok = der_expect(&in, DER_SEQUENCE, &seq) && in.len == 0 && der_expect(&seq, DER_SET, &set) &&
                  der_expect(&set, DER_OCTET_STRING, &os) && set.len == 0 && der_expect(&seq, DER_INTEGER, &num) && seq.len == 0;
        CHECK(ok && os.len == sizes[i] && memcmp(os.p, payload, sizes[i]) == 0, "length %u reads back", sizes[i]);
    }
    /* Minimal INTEGERs. */
    struct { uint8_t in[3]; uint32_t n; uint8_t out[4]; uint32_t on; } ints[] = {
        { {0x00, 0x00, 0x01}, 3, {0x02, 0x01, 0x01}, 3 },
        { {0x00, 0x80, 0x00}, 2, {0x02, 0x02, 0x00, 0x80}, 4 },
        { {0x00, 0x00, 0x00}, 3, {0x02, 0x01, 0x00}, 3 },
        { {0x7f, 0x00, 0x00}, 1, {0x02, 0x01, 0x7f}, 3 },
    };
    for (unsigned i = 0; i < 4; i++) {
        der_writer_t w;
        der_writer_init(&w, buf, sizeof buf);
        der_put_uint(&w, ints[i].in, ints[i].n);
        CHECK(!w.err && w.len == ints[i].on && memcmp(buf, ints[i].out, ints[i].on) == 0, "INTEGER %u", i);
    }
    /* Overflow sets err and writes nothing past the buffer. */
    der_writer_t w;
    der_writer_init(&w, buf, 10);
    der_begin(&w, DER_SEQUENCE);
    der_put(&w, DER_OCTET_STRING, payload, 20);
    der_end(&w);
    CHECK(w.err, "an overflowing write is an error");
    /* The reader refuses an indefinite length, a non-minimal one and one past the end. */
    static const uint8_t bad1[] = { 0x30, 0x80, 0x00, 0x00 }, bad2[] = { 0x30, 0x81, 0x05, 1, 2, 3, 4, 5 },
                         bad3[] = { 0x30, 0x05, 1, 2 };
    der_span_t s1 = { bad1, sizeof bad1 }, s2 = { bad2, sizeof bad2 }, s3 = { bad3, sizeof bad3 };
    CHECK(!der_next(&s1, NULL, NULL) && !der_next(&s2, NULL, NULL) && !der_next(&s3, NULL, NULL), "malformed lengths refused");
}

static void test_cert(void) {
    uint8_t der[1024], pub[64];
    uint32_t end = 0;
    int n = pem_decode(CERT_PEM, sizeof CERT_PEM - 1, "CERTIFICATE", der, sizeof der, &end);
    CHECK(n > 300 && end == sizeof CERT_PEM - 2, "PEM decodes (%d bytes, end %u)", n, end);
    tls_cert_info_t ci;
    CHECK(tls_cert_parse(der, (uint32_t)n, &ci) == TLS_OK, "the certificate parses");
    unhex(CERT_PUB_HEX, pub, 64);
    CHECK(memcmp(ci.pub, pub, 64) == 0, "its key is the one OpenSSL made");
    CHECK(strcmp(ci.subject_cn, "hosttest.local") == 0, "subject %s", ci.subject_cn);
    CHECK(strcmp(ci.issuer_cn, "Host Test CA") == 0, "issuer %s", ci.issuer_cn);
    CHECK(strcmp(ci.not_before, "261031133347Z") == 0 && strcmp(ci.not_after, "290202133347Z") == 0,
          "dates %s %s", ci.not_before, ci.not_after);

    /* A chain: the first block is the one returned, and `end` points past it. */
    char chain[2 * sizeof CERT_PEM];
    memcpy(chain, CERT_PEM, sizeof CERT_PEM - 1);
    memcpy(chain + sizeof CERT_PEM - 1, CERT_PEM, sizeof CERT_PEM);
    n = pem_decode(chain, (uint32_t)strlen(chain), "CERTIFICATE", der, sizeof der, &end);
    CHECK(n > 300 && end == sizeof CERT_PEM - 2, "the first block of a chain");
    CHECK(pem_decode(CERT_PEM, 200, "CERTIFICATE", der, sizeof der, NULL) < 0, "a cut-off block is not complete");

    /* Every single-byte change: parsed or refused, never a read outside the buffer. */
    n = pem_decode(CERT_PEM, sizeof CERT_PEM - 1, "CERTIFICATE", der, sizeof der, NULL);
    for (int i = 0; i < n; i++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t m[1024];
            memcpy(m, der, (size_t)n);
            m[i] ^= (uint8_t)(1u << bit);
            (void)tls_cert_parse(m, (uint32_t)n, &ci);
        }
        (void)tls_cert_parse(der, (uint32_t)i, &ci);          /* every truncation too */
    }
}

/* r||s from DER's ECDSA-Sig-Value. */
static bool sig_from_der(der_span_t v, uint8_t sig[64]) {
    der_span_t seq, r, s;
    if (!der_expect(&v, DER_SEQUENCE, &seq) || !der_expect(&seq, DER_INTEGER, &r) || !der_expect(&seq, DER_INTEGER, &s)) return false;
    memset(sig, 0, 64);
    while (r.len > 32 && r.p[0] == 0) { r.p++; r.len--; }
    while (s.len > 32 && s.p[0] == 0) { s.p++; s.len--; }
    if (r.len > 32 || s.len > 32) return false;
    memcpy(sig + 32 - r.len, r.p, r.len);
    memcpy(sig + 64 - s.len, s.p, s.len);
    return true;
}

static void test_csr(void) {
    uint8_t priv[32], pub[64], csr[1024];
    CHECK(p256_keypair(priv, pub) == CRYPTO_OK, "keypair");
    const uint8_t ip[4] = { 192, 168, 1, 7 };
    int n = tls_csr_build(priv, pub, "node.local", ip, csr, sizeof csr);
    CHECK(n > 200, "the request builds (%d)", n);
    if (n <= 0) return;
    /* CertificationRequest { info, algorithm, signature }: the signature verifies over info's bytes. */
    der_span_t in = { csr, (uint32_t)n }, outer, info, alg, bits;
    uint8_t tag;
    bool ok = der_expect(&in, DER_SEQUENCE, &outer) && in.len == 0 && der_next_raw(&outer, &info) &&
              der_expect(&outer, DER_SEQUENCE, &alg) && der_next(&outer, &tag, &bits) && tag == DER_BIT_STRING && outer.len == 0;
    CHECK(ok, "the request's three parts");
    if (!ok) return;
    uint8_t hash[32], sig[64];
    sha256(info.p, info.len, hash);
    der_span_t sv = { bits.p + 1, bits.len - 1 };
    CHECK(bits.p[0] == 0 && sig_from_der(sv, sig), "the signature decodes");
    CHECK(p256_verify(sig, pub, hash, 32) == CRYPTO_OK, "the signature verifies over the info");
    hash[0] ^= 1;
    CHECK(p256_verify(sig, pub, hash, 32) != CRYPTO_OK, "and not over anything else");
    /* The request carries the key and the names. */
    CHECK(memmem(csr, (size_t)n, pub, 64) != NULL, "the public key is in it");
    CHECK(memmem(csr, (size_t)n, "node.local", 10) != NULL && memmem(csr, (size_t)n, ip, 4) != NULL, "its names are in it");
    CHECK(memmem(csr, (size_t)n, priv, 32) == NULL, "the private key is not");
    char pem[2048];
    uint32_t pl = pem_encode("CERTIFICATE REQUEST", csr, (uint32_t)n, pem, sizeof pem);
    uint8_t back[1024];
    CHECK(pl > 0 && pem_decode(pem, pl, "CERTIFICATE REQUEST", back, sizeof back, NULL) == n && memcmp(back, csr, (size_t)n) == 0,
          "the request's PEM round-trips");
    if (getenv("DER_HOST_CSR_OUT")) {
        FILE *f = fopen(getenv("DER_HOST_CSR_OUT"), "w");
        if (f) { fwrite(pem, 1, pl, f); fclose(f); }
    }
    CHECK(tls_csr_build(priv, pub, "node.local", NULL, csr, 100) == TLS_ERR_MEMORY, "a request that does not fit");
}

static void test_random_inputs(unsigned iters) {
    uint8_t buf[600];
    tls_cert_info_t ci;
    for (unsigned it = 0; it < iters; it++) {
        uint32_t n = (uint32_t)(next() % sizeof buf);
        for (uint32_t i = 0; i < n; i++) buf[i] = (uint8_t)next();
        if (n > 4 && (next() & 1)) { buf[0] = 0x30; buf[1] = 0x82; buf[2] = (uint8_t)((n - 4) >> 8); buf[3] = (uint8_t)(n - 4); }
        (void)tls_cert_parse(buf, n, &ci);
        der_span_t s = { buf, n };
        uint8_t tag;
        der_span_t v;
        while (der_next(&s, &tag, &v)) { }
    }
}

int main(int argc, char **argv) {
    unsigned iters = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 2000;
    g_state = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x40c0ffee;
    test_base64();
    test_der_lengths();
    test_cert();
    for (int i = 0; i < 64; i++) test_csr();     /* r and s both get a 0x00 pad in about one of four */
    test_random_inputs(iters);
    if (fails) { printf("der_host: %d failure(s)\n", fails); return 1; }
    printf("der_host: base64, DER lengths and integers, an OpenSSL certificate, a request that verifies, %u random inputs: ok\n", iters);
    return 0;
}
