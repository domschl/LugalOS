/* A TLS 1.3 server: ChaCha20-Poly1305, X25519, ECDSA P-256 (net/include/net/tls.h). The section
 * numbers are RFC 8446's. */

#include "net/tls.h"
#include "kernel/crypto.h"
#include "kernel/der.h"
#include "kernel/random.h"
#include <string.h>

enum { ST_HELLO, ST_WAIT_FIN, ST_OPEN, ST_CLOSED, ST_FAILED };

#define CT_CCS        20
#define CT_ALERT      21
#define CT_HANDSHAKE  22
#define CT_APP        23

#define HS_CLIENT_HELLO  1
#define HS_SERVER_HELLO  2
#define HS_ENC_EXT       8
#define HS_CERTIFICATE  11
#define HS_CERT_VERIFY  15
#define HS_FINISHED     20
#define HS_KEY_UPDATE   24
#define HS_MESSAGE_HASH 254

#define EXT_SUPPORTED_GROUPS   10
#define EXT_SIG_ALGS           13
#define EXT_SUPPORTED_VERSIONS 43
#define EXT_KEY_SHARE          51

#define AL_CLOSE_NOTIFY     0
#define AL_UNEXPECTED      10
#define AL_BAD_MAC         20
#define AL_RECORD_OVERFLOW 22
#define AL_HS_FAILURE      40
#define AL_ILLEGAL_PARAM   47
#define AL_DECODE          50
#define AL_DECRYPT         51
#define AL_PROTOCOL_VER    70
#define AL_INTERNAL        80

#define SUITE_CHACHA   0x1303u
#define GROUP_X25519   0x001du
#define SIG_ECDSA_P256 0x0403u

/* §4.1.3: the random of a HelloRetryRequest, SHA-256("HelloRetryRequest"). */
static const uint8_t HRR_RANDOM[32] = {
    0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
    0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c,
};

/* --- the key schedule (§7) ------------------------------------------------------------------- */

/* HKDF-Expand-Label(secret, label, context, length) (§7.1). */
static void expand_label(const uint8_t secret[32], const char *label, const uint8_t *ctx, uint32_t ctx_len,
                         uint8_t *out, uint32_t out_len) {
    uint8_t info[2 + 1 + 6 + 24 + 1 + 32];
    uint32_t ll = (uint32_t)strlen(label), n = 0;
    info[n++] = (uint8_t)(out_len >> 8);
    info[n++] = (uint8_t)out_len;
    info[n++] = (uint8_t)(6 + ll);
    memcpy(info + n, "tls13 ", 6); n += 6;
    memcpy(info + n, label, ll); n += ll;
    info[n++] = (uint8_t)ctx_len;
    if (ctx_len) { memcpy(info + n, ctx, ctx_len); n += ctx_len; }
    (void)hkdf_sha256_expand(secret, info, n, out, out_len);
}

static void derive_secret(const uint8_t secret[32], const char *label, const uint8_t hash[32], uint8_t out[32]) {
    expand_label(secret, label, hash, 32, out, 32);
}

static void set_keys(const uint8_t secret[32], uint8_t key[32], uint8_t iv[12]) {
    expand_label(secret, "key", NULL, 0, key, 32);
    expand_label(secret, "iv", NULL, 0, iv, 12);
}

static void transcript_hash(const tls_t *t, uint8_t out[32]) {
    sha256_ctx_t c = t->transcript;
    sha256_final(&c, out);
}

/* §5.3: the per-record nonce, the IV with the sequence number XORed into its last 8 bytes. */
static void make_nonce(const uint8_t iv[12], uint64_t seq, uint8_t out[12]) {
    memcpy(out, iv, 12);
    for (int i = 0; i < 8; i++) out[11 - i] ^= (uint8_t)(seq >> (8 * i));
}

/* --- records out ------------------------------------------------------------------------------ */

static void put_u16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_u24(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }

static bool put_plain(tls_t *t, uint8_t type, const uint8_t *data, uint32_t len) {
    if (t->tx_len + 5 + len > TLS_TX_CAP) return false;
    uint8_t *p = t->tx + t->tx_len;
    p[0] = type; p[1] = 3; p[2] = 3;
    put_u16(p + 3, len);
    memcpy(p + 5, data, len);
    t->tx_len += 5 + len;
    return true;
}

/* An encrypted record is built in place: the plaintext at enc_body(), at most enc_room() bytes,
 * then enc_seal() adds the inner type, encrypts and writes the header. */
static uint8_t *enc_body(tls_t *t) { return t->tx + t->tx_len + 5; }
static uint32_t enc_room(const tls_t *t) {
    uint32_t used = t->tx_len + 5 + 1 + CHACHAPOLY_TAG_LEN;
    return used < TLS_TX_CAP ? TLS_TX_CAP - used : 0;
}
static void enc_seal(tls_t *t, uint32_t len, uint8_t inner_type) {
    uint8_t *h = t->tx + t->tx_len, *body = h + 5;
    body[len] = inner_type;
    uint32_t ct_len = len + 1;
    h[0] = CT_APP; h[1] = 3; h[2] = 3;
    put_u16(h + 3, ct_len + CHACHAPOLY_TAG_LEN);
    uint8_t nonce[12];
    make_nonce(t->s_iv, t->s_seq++, nonce);
    chachapoly_seal(body, body + ct_len, t->s_key, nonce, h, 5, body, ct_len);
    t->tx_len += 5 + ct_len + CHACHAPOLY_TAG_LEN;
}

bool tls_flush(tls_t *t) {
    while (t->tx_sent < t->tx_len) {
        int n = t->send(t->io, t->tx + t->tx_sent, t->tx_len - t->tx_sent);
        if (n == 0) return false;
        if (n < 0) {                                   /* the connection is gone: nothing is owed any more */
            if (t->st != ST_CLOSED) t->st = ST_FAILED;
            break;
        }
        t->tx_sent += (uint32_t)n;
    }
    t->tx_len = t->tx_sent = 0;
    return true;
}

/* Sends a fatal alert (§6.2) in place of anything still owed, and ends the session. */
static int fail(tls_t *t, uint8_t desc) {
    if (t->st == ST_FAILED || t->st == ST_CLOSED) return TLS_FAIL;
    t->alert = desc;
    t->tx_len = t->tx_sent = 0;
    uint8_t a[2] = { 2, desc };
    if (t->s_enc) { memcpy(enc_body(t), a, 2); enc_seal(t, 2, CT_ALERT); }
    else (void)put_plain(t, CT_ALERT, a, 2);
    (void)tls_flush(t);
    t->st = ST_FAILED;
    return TLS_FAIL;
}

/* --- reading a ClientHello ---------------------------------------------------------------- */

typedef struct { const uint8_t *p; uint32_t n; } span_t;

static bool rd_u8(span_t *s, uint32_t *v) { if (s->n < 1) return false; *v = s->p[0]; s->p++; s->n--; return true; }
static bool rd_u16(span_t *s, uint32_t *v) {
    if (s->n < 2) return false;
    *v = (uint32_t)s->p[0] << 8 | s->p[1];
    s->p += 2; s->n -= 2;
    return true;
}
static bool rd_bytes(span_t *s, uint32_t len, span_t *out) {
    if (s->n < len) return false;
    out->p = s->p; out->n = len;
    s->p += len; s->n -= len;
    return true;
}
static bool rd_vec8(span_t *s, span_t *out) { uint32_t l; return rd_u8(s, &l) && rd_bytes(s, l, out); }
static bool rd_vec16(span_t *s, span_t *out) { uint32_t l; return rd_u16(s, &l) && rd_bytes(s, l, out); }

/* True when a list of 16-bit values (each `vec`'s entries) holds `want`. */
static bool list16_has(span_t list, uint32_t want) {
    uint32_t v;
    while (rd_u16(&list, &v)) if (v == want) return true;
    return false;
}

static uint32_t server_hello(const tls_t *t, uint8_t *out, const uint8_t *random, const uint8_t *pub) {
    uint32_t o = 4;
    out[0] = HS_SERVER_HELLO;
    out[o++] = 3; out[o++] = 3;                        /* legacy_version */
    if (random) memcpy(out + o, random, 32);
    else random_bytes(out + o, 32);
    o += 32;
    out[o++] = t->sid_len;
    memcpy(out + o, t->sid, t->sid_len); o += t->sid_len;
    put_u16(out + o, SUITE_CHACHA); o += 2;
    out[o++] = 0;                                      /* compression */
    uint32_t ext_at = o;
    o += 2;
    put_u16(out + o, EXT_SUPPORTED_VERSIONS); put_u16(out + o + 2, 2); put_u16(out + o + 4, 0x0304); o += 6;
    put_u16(out + o, EXT_KEY_SHARE);
    if (pub) {
        put_u16(out + o + 2, 36); put_u16(out + o + 4, GROUP_X25519); put_u16(out + o + 6, 32);
        memcpy(out + o + 8, pub, 32);
        o += 40;
    } else {                                           /* HelloRetryRequest: the group only */
        put_u16(out + o + 2, 2); put_u16(out + o + 4, GROUP_X25519);
        o += 6;
    }
    put_u16(out + ext_at, o - ext_at - 2);
    put_u24(out + 1, o - 4);
    return o;
}

/* Middlebox compatibility (§D.4): one ChangeCipherSpec after the first ServerHello or HRR, when the
 * client sent a session id. */
static void compat_ccs(tls_t *t) {
    static const uint8_t one = 1;
    if (t->sid_len && !t->ccs_sent) { (void)put_plain(t, CT_CCS, &one, 1); t->ccs_sent = true; }
}

static int send_hrr(tls_t *t, const uint8_t *ch, uint32_t n) {
    uint8_t h[32], sh[128];
    sha256(ch, n, h);
    /* §4.4.1: the transcript restarts with a synthetic message_hash holding the first ClientHello's hash. */
    static const uint8_t mh[4] = { HS_MESSAGE_HASH, 0, 0, 32 };
    sha256_init(&t->transcript);
    sha256_update(&t->transcript, mh, 4);
    sha256_update(&t->transcript, h, 32);
    uint32_t k = server_hello(t, sh, HRR_RANDOM, NULL);
    sha256_update(&t->transcript, sh, k);
    if (!put_plain(t, CT_HANDSHAKE, sh, k)) return fail(t, AL_INTERNAL);
    compat_ccs(t);
    t->hrr_sent = true;
    (void)tls_flush(t);
    return 0;
}

/* The server's encrypted flight: EncryptedExtensions, Certificate, CertificateVerify, Finished, one
 * record. Updates the transcript as it goes. Returns false if it does not fit or cannot be signed. */
static bool server_flight(tls_t *t) {
    uint8_t *b = enc_body(t);
    uint32_t room = enc_room(t), o = 0;
    uint32_t cert_body = 1 + 3 + 3 + t->cert_len + 2;
    if (6 + 4 + cert_body + 4 + 4 + 72 + 36 > room) return false;

    static const uint8_t ee[6] = { HS_ENC_EXT, 0, 0, 2, 0, 0 };
    memcpy(b, ee, 6);
    sha256_update(&t->transcript, b, 6);
    o = 6;

    b[o] = HS_CERTIFICATE;
    put_u24(b + o + 1, cert_body);
    b[o + 4] = 0;                                      /* certificate_request_context */
    put_u24(b + o + 5, 3 + t->cert_len + 2);           /* certificate_list */
    put_u24(b + o + 8, t->cert_len);
    memcpy(b + o + 11, t->cert, t->cert_len);
    b[o + 11 + t->cert_len] = 0;                       /* no extensions */
    b[o + 12 + t->cert_len] = 0;
    sha256_update(&t->transcript, b + o, 4 + cert_body);
    o += 4 + cert_body;

    /* §4.4.3: the signature covers 64 spaces, the context string, a zero byte and the transcript hash. */
    uint8_t content[64 + 33 + 1 + 32], h[32], sig[P256_SIG_LEN], der[72];
    memset(content, 0x20, 64);
    memcpy(content + 64, "TLS 1.3, server CertificateVerify", 33);
    content[97] = 0;
    transcript_hash(t, content + 98);
    sha256(content, sizeof content, h);
    if (p256_sign(sig, t->priv, h, 32) != CRYPTO_OK) return false;
    der_writer_t w;
    der_writer_init(&w, der, sizeof der);
    der_begin(&w, DER_SEQUENCE);
      der_put_uint(&w, sig, 32);
      der_put_uint(&w, sig + 32, 32);
    der_end(&w);
    crypto_wipe_bytes(sig, sizeof sig);
    if (w.err) return false;
    b[o] = HS_CERT_VERIFY;
    put_u24(b + o + 1, 4 + w.len);
    put_u16(b + o + 4, SIG_ECDSA_P256);
    put_u16(b + o + 6, w.len);
    memcpy(b + o + 8, der, w.len);
    sha256_update(&t->transcript, b + o, 8 + w.len);
    o += 8 + w.len;

    uint8_t fk[32];
    expand_label(t->s_secret, "finished", NULL, 0, fk, 32);
    transcript_hash(t, h);
    b[o] = HS_FINISHED;
    put_u24(b + o + 1, 32);
    hmac_sha256(fk, 32, h, 32, b + o + 4);
    sha256_update(&t->transcript, b + o, 36);
    o += 36;
    crypto_wipe_bytes(fk, sizeof fk);

    enc_seal(t, o, CT_HANDSHAKE);
    return true;
}

static int client_hello(tls_t *t, const uint8_t *m, uint32_t n) {
    span_t r = { m + 4, n - 4 }, rnd, sid, suites, comp, exts;
    uint32_t legacy;
    if (!rd_u16(&r, &legacy) || !rd_bytes(&r, 32, &rnd) || !rd_vec8(&r, &sid) || sid.n > 32 ||
        !rd_vec16(&r, &suites) || !rd_vec8(&r, &comp) || !rd_vec16(&r, &exts) || r.n != 0)
        return fail(t, AL_DECODE);

    bool v13 = false, sig_ok = false, group_ok = false, null_comp = false;
    const uint8_t *share = NULL;
    for (uint32_t i = 0; i < comp.n; i++) if (comp.p[i] == 0) null_comp = true;
    while (exts.n) {
        uint32_t type;
        span_t data, list;
        if (!rd_u16(&exts, &type) || !rd_vec16(&exts, &data)) return fail(t, AL_DECODE);
        switch (type) {
        case EXT_SUPPORTED_VERSIONS:
            if (!rd_vec8(&data, &list)) return fail(t, AL_DECODE);
            v13 = list16_has(list, 0x0304);
            break;
        case EXT_SIG_ALGS:
            if (!rd_vec16(&data, &list)) return fail(t, AL_DECODE);
            sig_ok = list16_has(list, SIG_ECDSA_P256);
            break;
        case EXT_SUPPORTED_GROUPS:
            if (!rd_vec16(&data, &list)) return fail(t, AL_DECODE);
            group_ok = list16_has(list, GROUP_X25519);
            break;
        case EXT_KEY_SHARE:
            if (!rd_vec16(&data, &list)) return fail(t, AL_DECODE);
            while (list.n) {
                uint32_t g;
                span_t kx;
                if (!rd_u16(&list, &g) || !rd_vec16(&list, &kx)) return fail(t, AL_DECODE);
                if (g == GROUP_X25519 && kx.n == 32) share = kx.p;
            }
            break;
        default:
            break;                                       /* server_name, ALPN, PSK and the rest: not used */
        }
    }
    if (!v13) return fail(t, AL_PROTOCOL_VER);
    if (!list16_has(suites, SUITE_CHACHA) || !sig_ok || !null_comp) return fail(t, AL_HS_FAILURE);
    memcpy(t->sid, sid.p, sid.n);
    t->sid_len = (uint8_t)sid.n;
    if (!share) {
        if (!group_ok) return fail(t, AL_HS_FAILURE);
        if (t->hrr_sent) return fail(t, AL_ILLEGAL_PARAM);   /* asked for X25519 and still did not get it */
        return send_hrr(t, m, n);
    }
    sha256_update(&t->transcript, m, n);

    uint8_t eph[32], pub[32], shared[32];
    if (x25519_keypair(eph, pub) != CRYPTO_OK) return fail(t, AL_INTERNAL);
    int rc = x25519_shared(shared, eph, share);
    crypto_wipe_bytes(eph, sizeof eph);
    if (rc != CRYPTO_OK) return fail(t, AL_ILLEGAL_PARAM);

    uint8_t sh[128];                                  /* 122 with a 32-byte session id */
    uint32_t k = server_hello(t, sh, NULL, pub);
    sha256_update(&t->transcript, sh, k);
    if (!put_plain(t, CT_HANDSHAKE, sh, k)) return fail(t, AL_INTERNAL);
    compat_ccs(t);

    /* §7.1: early secret (no PSK) -> handshake secret (the X25519 result) -> master secret. */
    uint8_t zeros[32] = { 0 }, empty[32], s[32], derived[32], hs[32], master[32], h[32];
    sha256("", 0, empty);
    hkdf_sha256_extract(NULL, 0, zeros, 32, s);
    derive_secret(s, "derived", empty, derived);
    hkdf_sha256_extract(derived, 32, shared, 32, hs);
    crypto_wipe_bytes(shared, sizeof shared);
    transcript_hash(t, h);
    derive_secret(hs, "c hs traffic", h, t->c_secret);
    derive_secret(hs, "s hs traffic", h, t->s_secret);
    set_keys(t->c_secret, t->c_key, t->c_iv);
    set_keys(t->s_secret, t->s_key, t->s_iv);
    t->c_seq = t->s_seq = 0;
    t->c_enc = t->s_enc = true;
    derive_secret(hs, "derived", empty, derived);
    hkdf_sha256_extract(derived, 32, zeros, 32, master);
    crypto_wipe_bytes(hs, sizeof hs);

    if (!server_flight(t)) { crypto_wipe_bytes(master, sizeof master); return fail(t, AL_INTERNAL); }

    /* The application secrets hash the transcript through the server's Finished; so does the client's
     * Finished, which is still under the client's handshake key. */
    uint8_t fk[32];
    transcript_hash(t, h);
    derive_secret(master, "c ap traffic", h, t->c_ap);
    derive_secret(master, "s ap traffic", h, t->s_secret);
    expand_label(t->c_secret, "finished", NULL, 0, fk, 32);
    hmac_sha256(fk, 32, h, 32, t->expect_fin);
    set_keys(t->s_secret, t->s_key, t->s_iv);
    t->s_seq = 0;
    crypto_wipe_bytes(master, sizeof master);
    crypto_wipe_bytes(fk, sizeof fk);
    crypto_wipe_bytes(s, sizeof s);
    crypto_wipe_bytes(derived, sizeof derived);
    t->st = ST_WAIT_FIN;
    (void)tls_flush(t);
    return 0;
}

static void next_secret(uint8_t secret[32]) {
    uint8_t n[32];
    expand_label(secret, "traffic upd", NULL, 0, n, 32);
    memcpy(secret, n, 32);
    crypto_wipe_bytes(n, sizeof n);
}

static int handshake_msg(tls_t *t, const uint8_t *m, uint32_t n, bool enc) {
    uint8_t type = m[0];
    if (t->st == ST_HELLO && !enc && type == HS_CLIENT_HELLO) return client_hello(t, m, n);
    if (t->st == ST_WAIT_FIN && enc && type == HS_FINISHED) {
        if (n != 4 + 32 || !crypto_equal(m + 4, t->expect_fin, 32)) return fail(t, AL_DECRYPT);
        memcpy(t->c_secret, t->c_ap, 32);
        crypto_wipe_bytes(t->c_ap, sizeof t->c_ap);
        set_keys(t->c_secret, t->c_key, t->c_iv);
        t->c_seq = 0;
        t->st = ST_OPEN;
        return 0;
    }
    if (t->st == ST_OPEN && enc && type == HS_KEY_UPDATE) {             /* §4.6.3 */
        if (n != 5 || m[4] > 1) return fail(t, AL_DECODE);
        next_secret(t->c_secret);
        set_keys(t->c_secret, t->c_key, t->c_iv);
        t->c_seq = 0;
        if (m[4] == 1) {
            static const uint8_t ku[5] = { HS_KEY_UPDATE, 0, 0, 1, 0 };
            if (enc_room(t) < sizeof ku) return fail(t, AL_INTERNAL);
            memcpy(enc_body(t), ku, sizeof ku);
            enc_seal(t, sizeof ku, CT_HANDSHAKE);
            next_secret(t->s_secret);
            set_keys(t->s_secret, t->s_key, t->s_iv);
            t->s_seq = 0;
            (void)tls_flush(t);
        }
        return 0;
    }
    return fail(t, AL_UNEXPECTED);
}

/* --- records in ------------------------------------------------------------------------------ */

/* 1: a whole record is in rx; 0: not yet; TLS_EOF or TLS_FAIL. */
static int pull_record(tls_t *t) {
    for (;;) {
        uint32_t want;
        if (t->rx_len < 5) {
            want = 5 - t->rx_len;
        } else {
            uint32_t body = (uint32_t)t->rx[3] << 8 | t->rx[4];
            if (body > 16384u + 256u) return fail(t, AL_RECORD_OVERFLOW);
            if (t->rx_len == 5 + body) return 1;
            want = 5 + body - t->rx_len;
        }
        int n = t->recv(t->io, t->rx + t->rx_len, want);
        if (n < 0) { if (t->st != ST_FAILED) t->st = ST_CLOSED; return TLS_EOF; }
        if (n == 0) return 0;
        t->rx_len += (uint32_t)n;
    }
}

static int process_record(tls_t *t) {
    uint8_t type = t->rx[0];
    uint8_t *body = t->rx + 5;
    uint32_t len = t->rx_len - 5;
    bool enc = false;
    if (type == CT_CCS) {                    /* §5: a client's compatibility CCS, until it has finished */
        if (len != 1 || body[0] != 1 || t->st == ST_OPEN) return fail(t, AL_UNEXPECTED);
        t->rx_len = 0;
        return 0;
    }
    if (t->c_enc) {
        if (type != CT_APP) return fail(t, AL_UNEXPECTED);
        if (len < 1 + CHACHAPOLY_TAG_LEN) return fail(t, AL_BAD_MAC);
        uint8_t nonce[12];
        make_nonce(t->c_iv, t->c_seq, nonce);
        len -= CHACHAPOLY_TAG_LEN;
        if (chachapoly_open(body, t->c_key, nonce, t->rx, 5, body, len, body + len) != CRYPTO_OK)
            return fail(t, AL_BAD_MAC);
        t->c_seq++;
        while (len > 0 && body[len - 1] == 0) len--;               /* §5.2: padding */
        if (len == 0) return fail(t, AL_UNEXPECTED);
        type = body[--len];
        if (len > 16384u) return fail(t, AL_RECORD_OVERFLOW);
        enc = true;
    } else if (type != CT_HANDSHAKE && type != CT_ALERT) {
        return fail(t, AL_UNEXPECTED);
    }
    if (type == CT_ALERT) {
        t->rx_len = 0;
        if (len != 2) return fail(t, AL_DECODE);
        if (body[1] == AL_CLOSE_NOTIFY) { t->st = ST_CLOSED; return TLS_EOF; }
        t->alert = body[1];
        t->st = ST_FAILED;
        return TLS_FAIL;
    }
    if (type == CT_HANDSHAKE) {
        if (len == 0) return fail(t, AL_UNEXPECTED);
        for (uint32_t off = 0; off < len;) {
            if (len - off < 4) return fail(t, AL_UNEXPECTED);     /* a message split across records */
            uint32_t mlen = (uint32_t)body[off + 1] << 16 | (uint32_t)body[off + 2] << 8 | body[off + 3];
            if (mlen > len - off - 4) return fail(t, AL_UNEXPECTED);
            int rc = handshake_msg(t, body + off, 4 + mlen, enc);
            if (rc < 0) return rc;
            off += 4 + mlen;
        }
        t->rx_len = 0;
        return 0;
    }
    if (type == CT_APP && t->st == ST_OPEN) {
        if (len == 0) { t->rx_len = 0; return 0; }
        t->app_off = 5;
        t->app_end = 5 + len;
        return 0;
    }
    return fail(t, AL_UNEXPECTED);
}

/* --- the interface ---------------------------------------------------------------------------- */

void tls_server_init(tls_t *t, const uint8_t *cert, uint32_t cert_len, const uint8_t priv[32],
                     tls_recv_fn recv, tls_send_fn send, void *io, uint8_t *rx, uint8_t *tx) {
    memset(t, 0, sizeof *t);
    t->cert = cert;
    t->cert_len = cert_len;
    t->priv = priv;
    t->recv = recv;
    t->send = send;
    t->io = io;
    t->rx = rx;
    t->tx = tx;
    t->st = ST_HELLO;
    sha256_init(&t->transcript);
}

int tls_read(tls_t *t, uint8_t *buf, uint32_t cap) {
    for (int guard = 0; guard < 16; guard++) {
        if (t->app_off < t->app_end) {
            uint32_t k = t->app_end - t->app_off;
            if (k > cap) k = cap;
            memcpy(buf, t->rx + t->app_off, k);
            t->app_off += k;
            if (t->app_off == t->app_end) { t->app_off = t->app_end = 0; t->rx_len = 0; }
            return (int)k;
        }
        if (t->st == ST_CLOSED) return TLS_EOF;
        if (t->st == ST_FAILED) return TLS_FAIL;
        /* During the handshake our flight goes out before more of the client's is read: tx then never
         * holds more than one flight. */
        if (t->st != ST_OPEN && !tls_flush(t)) return 0;
        int r = pull_record(t);
        if (r <= 0) return r;
        r = process_record(t);
        if (r < 0) return r;
    }
    return 0;
}

int tls_write(tls_t *t, const uint8_t *buf, uint32_t len) {
    if (t->st == ST_FAILED || t->st == ST_CLOSED) return TLS_FAIL;
    if (!tls_flush(t) || t->st != ST_OPEN) return t->st == ST_FAILED ? TLS_FAIL : 0;
    uint32_t k = len < TLS_SEND_MAX ? len : TLS_SEND_MAX;
    if (k > enc_room(t)) k = enc_room(t);
    memcpy(enc_body(t), buf, k);
    enc_seal(t, k, CT_APP);
    (void)tls_flush(t);
    return (int)k;
}

bool tls_established(const tls_t *t) { return t->st == ST_OPEN; }

void tls_close(tls_t *t) {
    if (t->s_enc && (t->st == ST_OPEN || t->st == ST_WAIT_FIN) && enc_room(t) >= 2) {
        static const uint8_t a[2] = { 1, AL_CLOSE_NOTIFY };
        memcpy(enc_body(t), a, 2);
        enc_seal(t, 2, CT_ALERT);
        (void)tls_flush(t);
    }
    if (t->st != ST_FAILED) t->st = ST_CLOSED;
}

void tls_wipe(tls_t *t) {
    crypto_wipe_bytes(t->c_secret, sizeof t->c_secret);
    crypto_wipe_bytes(t->s_secret, sizeof t->s_secret);
    crypto_wipe_bytes(t->c_key, sizeof t->c_key);
    crypto_wipe_bytes(t->s_key, sizeof t->s_key);
    crypto_wipe_bytes(t->c_ap, sizeof t->c_ap);
    crypto_wipe_bytes(t->expect_fin, sizeof t->expect_fin);
}
