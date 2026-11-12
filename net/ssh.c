/*
 * Freestanding SSH-2 client (RFC 4251-4254, curve25519-sha256,
 * chacha20-poly1305@openssh.com, ssh-ed25519 / ecdsa-sha2-nistp256).
 * Zero static writable storage.
 */

#include "net/ssh.h"

#if defined(LUGALOS_HOST_TEST) || defined(CONFIG_SSH_CLIENT)

#include "kernel/crypto.h"
#include "kernel/sha256.h"
#include "third_party/crypto/monocypher/monocypher.h"

#include <string.h>

#if defined(LUGALOS_HOST_TEST)
#include <stdlib.h>
#include <stdio.h>
void random_bytes(void *buf, uint32_t len);
uint64_t time_get_ms(void);
static inline void ssh_yield(void) {}
#else
#include "net/dns.h"
#include "net/tcp.h"
#include "kernel/palloc.h"
#include "kernel/random.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/keyseq.h"
#include "fs/vfs.h"
static inline void ssh_yield(void) { sched_yield(); }
#endif

/* SSH message numbers */
#define SSH_MSG_DISCONNECT          1u
#define SSH_MSG_IGNORE              2u
#define SSH_MSG_UNIMPLEMENTED       3u
#define SSH_MSG_DEBUG               4u
#define SSH_MSG_SERVICE_REQUEST     5u
#define SSH_MSG_SERVICE_ACCEPT      6u
#define SSH_MSG_EXT_INFO            7u
#define SSH_MSG_KEXINIT             20u
#define SSH_MSG_NEWKEYS             21u
#define SSH_MSG_KEX_ECDH_INIT       30u
#define SSH_MSG_KEX_ECDH_REPLY      31u
#define SSH_MSG_USERAUTH_REQUEST    50u
#define SSH_MSG_USERAUTH_FAILURE    51u
#define SSH_MSG_USERAUTH_SUCCESS    52u
#define SSH_MSG_GLOBAL_REQUEST      80u
#define SSH_MSG_REQUEST_SUCCESS     81u
#define SSH_MSG_REQUEST_FAILURE     82u
#define SSH_MSG_CHANNEL_OPEN        90u
#define SSH_MSG_CHANNEL_OPEN_CONF   91u
#define SSH_MSG_CHANNEL_OPEN_FAIL   92u
#define SSH_MSG_CHANNEL_WIN_ADJ     93u
#define SSH_MSG_CHANNEL_DATA        94u
#define SSH_MSG_CHANNEL_EXT_DATA    95u
#define SSH_MSG_CHANNEL_EOF         96u
#define SSH_MSG_CHANNEL_CLOSE       97u
#define SSH_MSG_CHANNEL_REQUEST     98u
#define SSH_MSG_CHANNEL_SUCCESS     99u
#define SSH_MSG_CHANNEL_FAILURE     100u

#define SSH_CLIENT_BANNER "SSH-2.0-LugalOS_1.0"
#define SSH_MAX_PKT       35000u
#define SSH_BUF_CAP       (SSH_MAX_PKT + 64u)
#define SSH_LOCAL_WIN     (64u * 1024u)
#define SSH_LOCAL_MAXPKT  16384u

struct ssh_session {
    ssh_recv_fn recv;
    ssh_send_fn send;
    void       *io;
    bool        owns_tcp;
    uint32_t    timeout_ms;

    uint8_t     rx[SSH_BUF_CAP];
    uint8_t     tx[SSH_BUF_CAP];
    uint8_t     pkt[SSH_BUF_CAP];
    uint8_t     kex_c[2048];
    uint32_t    kex_c_len;
    uint8_t     kex_s[2048];
    uint32_t    kex_s_len;
    char        banner_s[256];
    uint32_t    banner_s_len;

    uint8_t     session_id[32];
    bool        have_session_id;
    uint8_t     c2s_key[64];
    uint8_t     s2c_key[64];
    bool        enc_c2s;
    bool        enc_s2c;
    uint32_t    tx_seq;
    uint32_t    rx_seq;

    /* Active channel state */
    bool        chan_open;
    bool        chan_eof;
    bool        chan_closed;
    uint32_t    local_chan;
    uint32_t    remote_chan;
    uint32_t    remote_win;
    uint32_t    remote_maxpkt;
    uint32_t    local_win_avail;
    int         exit_status;

    /* Pending decrypted channel data */
    uint8_t     chan_buf[SSH_BUF_CAP];
    uint8_t     chan_tx[4096u + 32u];
    uint32_t    chan_off;
    uint32_t    chan_len;
    bool        chan_is_stderr;

#if !defined(LUGALOS_HOST_TEST)
    tcp_stream_t tcp;
#endif
};

/* --- Wire encoding / decoding helpers --------------------------------------- */

static inline uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

typedef struct {
    const uint8_t *p;
    size_t         n;
} ssh_span_t;

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     err;
} ssh_wr_t;

static bool rd_u8(ssh_span_t *s, uint8_t *out) {
    if (s->n < 1u) return false;
    *out = *s->p++;
    s->n--;
    return true;
}

static bool rd_u32(ssh_span_t *s, uint32_t *out) {
    if (s->n < 4u) return false;
    *out = rd_be32(s->p);
    s->p += 4u;
    s->n -= 4u;
    return true;
}

static bool rd_str(ssh_span_t *s, ssh_span_t *out) {
    uint32_t l;
    if (!rd_u32(s, &l) || l > s->n) return false;
    out->p = s->p;
    out->n = l;
    s->p += l;
    s->n -= l;
    return true;
}

static void wr_u8(ssh_wr_t *w, uint8_t v) {
    if (w->err || w->len + 1u > w->cap) { w->err = true; return; }
    w->buf[w->len++] = v;
}

static void wr_u32(ssh_wr_t *w, uint32_t v) {
    if (w->err || w->len + 4u > w->cap) { w->err = true; return; }
    wr_be32(w->buf + w->len, v);
    w->len += 4u;
}

static void wr_bytes(ssh_wr_t *w, const void *p, size_t n) {
    if (w->err || w->len + n > w->cap) { w->err = true; return; }
    if (n > 0) memcpy(w->buf + w->len, p, n);
    w->len += n;
}

static void wr_str(ssh_wr_t *w, const void *p, size_t n) {
    wr_u32(w, (uint32_t)n);
    wr_bytes(w, p, n);
}

static void wr_cstr(ssh_wr_t *w, const char *s) {
    wr_str(w, s, strlen(s));
}

static void sha256_wr_str(sha256_ctx_t *h, const void *p, size_t n) {
    uint8_t lenb[4];
    wr_be32(lenb, (uint32_t)n);
    sha256_update(h, lenb, 4u);
    if (n > 0) sha256_update(h, p, n);
}

static void sha256_wr_mpint(sha256_ctx_t *h, const uint8_t *p, size_t n) {
    while (n > 0 && *p == 0) { p++; n--; }
    uint8_t lenb[4];
    if (n == 0) {
        wr_be32(lenb, 0);
        sha256_update(h, lenb, 4u);
        return;
    }
    uint8_t pad = (p[0] & 0x80u) ? 1u : 0u;
    wr_be32(lenb, (uint32_t)(n + pad));
    sha256_update(h, lenb, 4u);
    if (pad) {
        uint8_t z = 0;
        sha256_update(h, &z, 1u);
    }
    sha256_update(h, p, n);
}

/* --- Base64 helpers --------------------------------------------------------- */

static size_t b64_encode(const uint8_t *in, size_t len, char *out, size_t cap) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t pos = 0;
    for (size_t i = 0; i < len; i += 3u) {
        uint32_t a = in[i];
        uint32_t b = (i + 1u < len) ? in[i + 1u] : 0;
        uint32_t c = (i + 2u < len) ? in[i + 2u] : 0;
        uint32_t t = (a << 16) | (b << 8) | c;
        if (pos + 4u >= cap) return 0;
        out[pos++] = tbl[(t >> 18) & 63u];
        out[pos++] = tbl[(t >> 12) & 63u];
        out[pos++] = (i + 1u < len) ? tbl[(t >> 6) & 63u] : '=';
        out[pos++] = (i + 2u < len) ? tbl[t & 63u] : '=';
    }
    if (pos < cap) out[pos] = '\0';
    return pos;
}

size_t ssh_encode_ed25519_blob(const uint8_t pub[32], uint8_t *out, size_t cap) {
    ssh_wr_t w = { out, cap, 0, false };
    wr_cstr(&w, "ssh-ed25519");
    wr_str(&w, pub, 32u);
    return w.err ? 0 : w.len;
}

int ssh_format_ed25519_pub(const uint8_t pub[32], const char *comment,
                           char *out, size_t cap) {
    uint8_t blob[64];
    size_t blen = ssh_encode_ed25519_blob(pub, blob, sizeof(blob));
    char b64[128];
    size_t b64len = b64_encode(blob, blen, b64, sizeof(b64));
    if (!blen || !b64len) return SSH_ERR_INVAL;
    const char *c = (comment && comment[0]) ? comment : "lugalos";
    size_t need = 12u + b64len + 1u + strlen(c) + 2u;
    if (need > cap) return SSH_ERR_NOMEM;
    size_t pos = 0;
    memcpy(out + pos, "ssh-ed25519 ", 12u); pos += 12u;
    memcpy(out + pos, b64, b64len);        pos += b64len;
    out[pos++] = ' ';
    size_t clen = strlen(c);
    memcpy(out + pos, c, clen);            pos += clen;
    out[pos++] = '\n';
    out[pos] = '\0';
    return (int)pos;
}

int ssh_key_fingerprint_sha256(const uint8_t *blob, size_t blob_len,
                               char *out, size_t cap) {
    uint8_t h[32];
    sha256(blob, (uint32_t)blob_len, h);
    char b64[64];
    size_t bl = b64_encode(h, sizeof(h), b64, sizeof(b64));
    while (bl > 0 && b64[bl - 1u] == '=') b64[--bl] = '\0';
    if (7u + bl + 1u > cap) return SSH_ERR_NOMEM;
    memcpy(out, "SHA256:", 7u);
    memcpy(out + 7u, b64, bl + 1u);
    return (int)(7u + bl);
}

/* --- Session memory allocation ---------------------------------------------- */

static ssh_session_t *ssh_alloc_session(void) {
#if defined(LUGALOS_HOST_TEST)
    return (ssh_session_t *)calloc(1u, sizeof(ssh_session_t));
#else
    uint32_t pages = (uint32_t)((sizeof(ssh_session_t) + 4095u) / 4096u);
    ssh_session_t *s = (ssh_session_t *)palloc_pages_bulk(pages);
    if (s) memset(s, 0, pages * 4096u);
    return s;
#endif
}

static void ssh_free_session(ssh_session_t *s) {
    if (!s) return;
    crypto_wipe_bytes(s, sizeof(*s));
#if defined(LUGALOS_HOST_TEST)
    free(s);
#else
    uint32_t pages = (uint32_t)((sizeof(ssh_session_t) + 4095u) / 4096u);
    palloc_free(s, pages);
#endif
}

/* --- Blocking I/O with timeout over non-blocking callbacks ------------------ */

static int io_send_all(ssh_session_t *s, const uint8_t *buf, uint32_t len) {
    uint32_t sent = 0;
    uint64_t deadline = time_get_ms() + s->timeout_ms;
    while (sent < len) {
        int n = s->send(s->io, buf + sent, len - sent);
        if (n < 0) return SSH_ERR_NET;
        if (n > 0) {
            sent += (uint32_t)n;
            deadline = time_get_ms() + s->timeout_ms;
            continue;
        }
        if (time_get_ms() >= deadline) return SSH_ERR_TIMEOUT;
        ssh_yield();
    }
    return SSH_OK;
}

static int io_recv_exact(ssh_session_t *s, uint8_t *buf, uint32_t len) {
    uint32_t got = 0;
    uint64_t deadline = time_get_ms() + s->timeout_ms;
    while (got < len) {
        int n = s->recv(s->io, buf + got, len - got);
        if (n < 0) return SSH_EOF;
        if (n > 0) {
            got += (uint32_t)n;
            deadline = time_get_ms() + s->timeout_ms;
            continue;
        }
        if (time_get_ms() >= deadline) return SSH_ERR_TIMEOUT;
        ssh_yield();
    }
    return SSH_OK;
}

/* --- Packet Layer (plain + chacha20-poly1305@openssh.com) ------------------- */

static void chacha_seq_nonce(uint32_t seq, uint8_t nonce[12]) {
    memset(nonce, 0, 8u);
    wr_be32(nonce + 8u, seq);
}

static int ssh_send_pkt(ssh_session_t *s, const uint8_t *payload, uint32_t plen) {
    if (plen + 32u > sizeof(s->tx)) return SSH_ERR_INVAL;
    uint32_t pad = 8u - ((1u + plen) & 7u);
    if (pad < 4u) pad += 8u;
    uint32_t pkt_len = 1u + plen + pad;

    uint8_t *plain = s->pkt;
    plain[0] = (uint8_t)pad;
    if (plen > 0) memcpy(plain + 1u, payload, plen);
    random_bytes(plain + 1u + plen, pad);

    if (!s->enc_c2s) {
        wr_be32(s->tx, pkt_len);
        memcpy(s->tx + 4u, plain, pkt_len);
        s->tx_seq++;
        return io_send_all(s, s->tx, 4u + pkt_len);
    }

    uint8_t nonce[12];
    chacha_seq_nonce(s->tx_seq, nonce);

    uint8_t len_be[4];
    wr_be32(len_be, pkt_len);
    crypto_chacha20_ietf(s->tx, len_be, 4u, s->c2s_key + 32u, nonce, 0);

    uint8_t poly_key[64];
    memset(poly_key, 0, sizeof(poly_key));
    crypto_chacha20_ietf(poly_key, poly_key, sizeof(poly_key), s->c2s_key, nonce, 0);

    crypto_chacha20_ietf(s->tx + 4u, plain, pkt_len, s->c2s_key, nonce, 1);

    crypto_poly1305_ctx pctx;
    crypto_poly1305_init(&pctx, poly_key);
    crypto_poly1305_update(&pctx, s->tx, 4u + pkt_len);
    crypto_poly1305_final(&pctx, s->tx + 4u + pkt_len);
    crypto_wipe(poly_key, sizeof(poly_key));

    s->tx_seq++;
    return io_send_all(s, s->tx, 4u + pkt_len + 16u);
}

static int ssh_finish_pkt(ssh_session_t *s, const uint8_t hdr_enc[4], uint32_t *out_len) {
    uint32_t pkt_len = 0;
    int rc;
    if (!s->enc_s2c) {
        pkt_len = rd_be32(hdr_enc);
        if (pkt_len < 5u || pkt_len > SSH_MAX_PKT) return SSH_ERR_PROTO;
        rc = io_recv_exact(s, s->pkt, pkt_len);
        if (rc != SSH_OK) return rc;
        s->rx_seq++;
    } else {
        uint8_t nonce[12], len_plain[4];
        chacha_seq_nonce(s->rx_seq, nonce);
        crypto_chacha20_ietf(len_plain, hdr_enc, 4u, s->s2c_key + 32u, nonce, 0);
        pkt_len = rd_be32(len_plain);
        if (pkt_len < 5u || pkt_len > SSH_MAX_PKT) return SSH_ERR_PROTO;

        rc = io_recv_exact(s, s->rx, pkt_len + 16u);
        if (rc != SSH_OK) return rc;

        uint8_t poly_key[64], tag[16];
        memset(poly_key, 0, sizeof(poly_key));
        crypto_chacha20_ietf(poly_key, poly_key, sizeof(poly_key), s->s2c_key, nonce, 0);

        crypto_poly1305_ctx pctx;
        crypto_poly1305_init(&pctx, poly_key);
        crypto_poly1305_update(&pctx, hdr_enc, 4u);
        crypto_poly1305_update(&pctx, s->rx, pkt_len);
        crypto_poly1305_final(&pctx, tag);
        crypto_wipe(poly_key, sizeof(poly_key));

        if (crypto_verify16(tag, s->rx + pkt_len) != 0) return SSH_ERR_CRYPTO;
        crypto_chacha20_ietf(s->pkt, s->rx, pkt_len, s->s2c_key, nonce, 1);
        s->rx_seq++;
    }

    uint8_t pad = s->pkt[0];
    if (pad < 4u || (uint32_t)pad >= pkt_len) return SSH_ERR_PROTO;
    uint32_t payload_len = pkt_len - 1u - (uint32_t)pad;
    memmove(s->pkt, s->pkt + 1u, payload_len);
    *out_len = payload_len;
    return SSH_OK;
}

static int ssh_recv_pkt_raw(ssh_session_t *s, uint32_t *out_len) {
    uint8_t hdr_enc[4];
    int rc = io_recv_exact(s, hdr_enc, 4u);
    if (rc != SSH_OK) return rc;
    return ssh_finish_pkt(s, hdr_enc, out_len);
}

static int ssh_recv_pkt(ssh_session_t *s, uint32_t *out_len) {
    for (;;) {
        uint32_t len = 0;
        int rc = ssh_recv_pkt_raw(s, &len);
        if (rc != SSH_OK) return rc;
        if (len == 0) return SSH_ERR_PROTO;
        uint8_t type = s->pkt[0];
        if (type == SSH_MSG_IGNORE || type == SSH_MSG_DEBUG || type == SSH_MSG_EXT_INFO) {
            continue;
        }
        if (type == SSH_MSG_DISCONNECT) return SSH_EOF;
        if (type == SSH_MSG_GLOBAL_REQUEST) {
            ssh_span_t sp = { s->pkt + 1u, len - 1u }, rname;
            uint8_t want_reply = 0;
            if (rd_str(&sp, &rname) && rd_u8(&sp, &want_reply) && want_reply) {
                uint8_t rep[1] = { SSH_MSG_REQUEST_FAILURE };
                (void)ssh_send_pkt(s, rep, sizeof(rep));
            }
            continue;
        }
        *out_len = len;
        return SSH_OK;
    }
}

/* --- Key Exchange & Host Key Verification ----------------------------------- */

static bool name_list_has(ssh_span_t list, const char *target) {
    size_t tlen = strlen(target);
    size_t i = 0;
    while (i < list.n) {
        size_t start = i;
        while (i < list.n && list.p[i] != ',') i++;
        if (i - start == tlen && memcmp(list.p + start, target, tlen) == 0) return true;
        i++;
    }
    return false;
}

static void derive_key(const uint8_t *k_shared, const uint8_t h[32],
                       const uint8_t session_id[32], char letter, uint8_t out[64]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_wr_mpint(&ctx, k_shared, 32u);
    sha256_update(&ctx, h, 32u);
    uint8_t ch = (uint8_t)letter;
    sha256_update(&ctx, &ch, 1u);
    sha256_update(&ctx, session_id, 32u);
    sha256_final(&ctx, out);

    sha256_init(&ctx);
    sha256_wr_mpint(&ctx, k_shared, 32u);
    sha256_update(&ctx, h, 32u);
    sha256_update(&ctx, out, 32u);
    sha256_final(&ctx, out + 32u);
}

static bool verify_host_sig(const uint8_t *ks, size_t ks_len,
                            const uint8_t *sig_blob, size_t sig_len,
                            const uint8_t h[32], char *key_type_out, size_t kt_cap) {
    ssh_span_t ksp = { ks, ks_len }, ktype, kdata;
    if (!rd_str(&ksp, &ktype) || !rd_str(&ksp, &kdata)) return false;
    if (ktype.n >= kt_cap) return false;
    memcpy(key_type_out, ktype.p, ktype.n);
    key_type_out[ktype.n] = '\0';

    ssh_span_t ssp = { sig_blob, sig_len }, stype, sdata;
    if (!rd_str(&ssp, &stype) || !rd_str(&ssp, &sdata)) return false;

    if (ktype.n == 11u && memcmp(ktype.p, "ssh-ed25519", 11u) == 0) {
        if (kdata.n != 32u || sdata.n != 64u) return false;
        if (stype.n != 11u || memcmp(stype.p, "ssh-ed25519", 11u) != 0) return false;
        return ed25519_verify(sdata.p, kdata.p, h, 32u) == CRYPTO_OK;
    }

    if (ktype.n == 19u && memcmp(ktype.p, "ecdsa-sha2-nistp256", 19u) == 0) {
        ssh_span_t curve, pt;
        ksp = (ssh_span_t){ ks, ks_len };
        if (!rd_str(&ksp, &ktype) || !rd_str(&ksp, &curve) || !rd_str(&ksp, &pt)) return false;
        if (pt.n != 65u || pt.p[0] != 0x04u) return false;
        ssh_span_t r_mp, s_mp;
        if (!rd_str(&sdata, &r_mp) || !rd_str(&sdata, &s_mp)) return false;
        while (r_mp.n > 0 && r_mp.p[0] == 0) { r_mp.p++; r_mp.n--; }
        while (s_mp.n > 0 && s_mp.p[0] == 0) { s_mp.p++; s_mp.n--; }
        if (r_mp.n > 32u || s_mp.n > 32u) return false;
        uint8_t sig64[64];
        memset(sig64, 0, sizeof(sig64));
        memcpy(sig64 + 32u - r_mp.n, r_mp.p, r_mp.n);
        memcpy(sig64 + 64u - s_mp.n, s_mp.p, s_mp.n);
        uint8_t digest[32];
        sha256(h, 32u, digest);
        return p256_verify(sig64, pt.p + 1u, digest, sizeof(digest)) == CRYPTO_OK;
    }

    return false;
}

static int ssh_kex(ssh_session_t *s, const ssh_cfg_t *cfg) {
    /* 1. Banner exchange */
    const char *cbanner = SSH_CLIENT_BANNER "\r\n";
    int rc = io_send_all(s, (const uint8_t *)cbanner, (uint32_t)strlen(cbanner));
    if (rc != SSH_OK) return rc;

    for (unsigned line = 0; line < 16u; line++) {
        uint32_t pos = 0;
        while (pos < sizeof(s->banner_s) - 1u) {
            uint8_t ch = 0;
            rc = io_recv_exact(s, &ch, 1u);
            if (rc != SSH_OK) return rc;
            if (ch == '\n') break;
            if (ch != '\r') s->banner_s[pos++] = (char)ch;
        }
        s->banner_s[pos] = '\0';
        s->banner_s_len = pos;
        if (pos >= 8u && memcmp(s->banner_s, "SSH-2.0-", 8u) == 0) break;
        if (line == 15u) return SSH_ERR_PROTO;
    }

    /* 2. KEXINIT */
    uint8_t cookie[16];
    random_bytes(cookie, sizeof(cookie));
    ssh_wr_t w = { s->kex_c, sizeof(s->kex_c), 0, false };
    wr_u8(&w, SSH_MSG_KEXINIT);
    wr_bytes(&w, cookie, sizeof(cookie));
    wr_cstr(&w, "curve25519-sha256,curve25519-sha256@libssh.org");
    wr_cstr(&w, "ssh-ed25519,ecdsa-sha2-nistp256");
    wr_cstr(&w, "chacha20-poly1305@openssh.com");
    wr_cstr(&w, "chacha20-poly1305@openssh.com");
    wr_cstr(&w, "none");
    wr_cstr(&w, "none");
    wr_cstr(&w, "none");
    wr_cstr(&w, "none");
    wr_cstr(&w, "");
    wr_cstr(&w, "");
    wr_u8(&w, 0);
    wr_u32(&w, 0);
    if (w.err) return SSH_ERR_NOMEM;
    s->kex_c_len = (uint32_t)w.len;
    rc = ssh_send_pkt(s, s->kex_c, s->kex_c_len);
    if (rc != SSH_OK) return rc;

    uint32_t pkt_len = 0;
    rc = ssh_recv_pkt(s, &pkt_len);
    if (rc != SSH_OK) return rc;
    if (s->pkt[0] != SSH_MSG_KEXINIT || pkt_len > sizeof(s->kex_s)) return SSH_ERR_PROTO;
    memcpy(s->kex_s, s->pkt, pkt_len);
    s->kex_s_len = pkt_len;

    ssh_span_t ksp = { s->kex_s + 17u, pkt_len - 17u };
    ssh_span_t kex_algs, hk_algs, enc_c, enc_s;
    if (!rd_str(&ksp, &kex_algs) || !rd_str(&ksp, &hk_algs) ||
        !rd_str(&ksp, &enc_c) || !rd_str(&ksp, &enc_s)) return SSH_ERR_PROTO;
    if (!name_list_has(kex_algs, "curve25519-sha256") &&
        !name_list_has(kex_algs, "curve25519-sha256@libssh.org")) return SSH_ERR_PROTO;
    if (!name_list_has(enc_c, "chacha20-poly1305@openssh.com") ||
        !name_list_has(enc_s, "chacha20-poly1305@openssh.com")) return SSH_ERR_PROTO;

    /* 3. ECDH X25519 */
    uint8_t eph_priv[32], eph_pub[32];
    if (x25519_keypair(eph_priv, eph_pub) != CRYPTO_OK) return SSH_ERR_CRYPTO;

    uint8_t init_msg[64];
    ssh_wr_t iw = { init_msg, sizeof(init_msg), 0, false };
    wr_u8(&iw, SSH_MSG_KEX_ECDH_INIT);
    wr_str(&iw, eph_pub, sizeof(eph_pub));
    rc = ssh_send_pkt(s, init_msg, (uint32_t)iw.len);
    if (rc != SSH_OK) { crypto_wipe(eph_priv, sizeof(eph_priv)); return rc; }

    rc = ssh_recv_pkt(s, &pkt_len);
    if (rc != SSH_OK || s->pkt[0] != SSH_MSG_KEX_ECDH_REPLY) {
        crypto_wipe(eph_priv, sizeof(eph_priv));
        return rc != SSH_OK ? rc : SSH_ERR_PROTO;
    }

    ssh_span_t rsp = { s->pkt + 1u, pkt_len - 1u }, ks, qs, sig;
    if (!rd_str(&rsp, &ks) || !rd_str(&rsp, &qs) || !rd_str(&rsp, &sig) || qs.n != 32u) {
        crypto_wipe(eph_priv, sizeof(eph_priv));
        return SSH_ERR_PROTO;
    }

    uint8_t shared[32];
    if (x25519_shared(shared, eph_priv, qs.p) != CRYPTO_OK) {
        crypto_wipe(eph_priv, sizeof(eph_priv));
        return SSH_ERR_CRYPTO;
    }
    crypto_wipe(eph_priv, sizeof(eph_priv));

    uint8_t h[32];
    sha256_ctx_t hctx;
    sha256_init(&hctx);
    sha256_wr_str(&hctx, SSH_CLIENT_BANNER, strlen(SSH_CLIENT_BANNER));
    sha256_wr_str(&hctx, s->banner_s, s->banner_s_len);
    sha256_wr_str(&hctx, s->kex_c, s->kex_c_len);
    sha256_wr_str(&hctx, s->kex_s, s->kex_s_len);
    sha256_wr_str(&hctx, ks.p, ks.n);
    sha256_wr_str(&hctx, eph_pub, sizeof(eph_pub));
    sha256_wr_str(&hctx, qs.p, qs.n);
    sha256_wr_mpint(&hctx, shared, sizeof(shared));
    sha256_final(&hctx, h);

    char ktype[32];
    if (!verify_host_sig(ks.p, ks.n, sig.p, sig.n, h, ktype, sizeof(ktype))) {
        crypto_wipe(shared, sizeof(shared));
        return SSH_ERR_CRYPTO;
    }
    if (cfg->hostkey_cb) {
        uint16_t port = cfg->port ? cfg->port : SSH_DEFAULT_PORT;
        if (!cfg->hostkey_cb(cfg->hostkey_ctx, cfg->host, port, ktype, ks.p, ks.n)) {
            crypto_wipe(shared, sizeof(shared));
            return SSH_ERR_HOSTKEY;
        }
    }

    if (!s->have_session_id) {
        memcpy(s->session_id, h, 32u);
        s->have_session_id = true;
    }

    /* 4. NEWKEYS */
    uint8_t nk = SSH_MSG_NEWKEYS;
    rc = ssh_send_pkt(s, &nk, 1u);
    if (rc != SSH_OK) { crypto_wipe(shared, sizeof(shared)); return rc; }

    rc = ssh_recv_pkt(s, &pkt_len);
    if (rc != SSH_OK || s->pkt[0] != SSH_MSG_NEWKEYS) {
        crypto_wipe(shared, sizeof(shared));
        return rc != SSH_OK ? rc : SSH_ERR_PROTO;
    }

    derive_key(shared, h, s->session_id, 'C', s->c2s_key);
    derive_key(shared, h, s->session_id, 'D', s->s2c_key);
    crypto_wipe(shared, sizeof(shared));
    s->enc_c2s = true;
    s->enc_s2c = true;
    return SSH_OK;
}

/* --- User Authentication ---------------------------------------------------- */

static int ssh_userauth(ssh_session_t *s, const ssh_cfg_t *cfg) {
    uint8_t req[512];
    ssh_wr_t w = { req, sizeof(req), 0, false };
    wr_u8(&w, SSH_MSG_SERVICE_REQUEST);
    wr_cstr(&w, "ssh-userauth");
    int rc = ssh_send_pkt(s, req, (uint32_t)w.len);
    if (rc != SSH_OK) return rc;

    uint32_t pkt_len = 0;
    rc = ssh_recv_pkt(s, &pkt_len);
    if (rc != SSH_OK || s->pkt[0] != SSH_MSG_SERVICE_ACCEPT) {
        return rc != SSH_OK ? rc : SSH_ERR_PROTO;
    }

    /* Try Ed25519 publickey auth if seed provided */
    if (cfg->ed25519_seed) {
        uint8_t secret[64], pub[32], blob[64];
        ed25519_keypair_from_seed(secret, pub, cfg->ed25519_seed);
        size_t blen = ssh_encode_ed25519_blob(pub, blob, sizeof(blob));

        uint8_t to_sign[512];
        ssh_wr_t sw = { to_sign, sizeof(to_sign), 0, false };
        wr_str(&sw, s->session_id, 32u);
        wr_u8(&sw, SSH_MSG_USERAUTH_REQUEST);
        wr_cstr(&sw, cfg->user);
        wr_cstr(&sw, "ssh-connection");
        wr_cstr(&sw, "publickey");
        wr_u8(&sw, 1u);
        wr_cstr(&sw, "ssh-ed25519");
        wr_str(&sw, blob, blen);

        uint8_t sig[64];
        ed25519_sign(sig, secret, to_sign, (uint32_t)sw.len);
        crypto_wipe(secret, sizeof(secret));

        uint8_t sig_blob[96];
        ssh_wr_t sb = { sig_blob, sizeof(sig_blob), 0, false };
        wr_cstr(&sb, "ssh-ed25519");
        wr_str(&sb, sig, sizeof(sig));

        w = (ssh_wr_t){ req, sizeof(req), 0, false };
        wr_u8(&w, SSH_MSG_USERAUTH_REQUEST);
        wr_cstr(&w, cfg->user);
        wr_cstr(&w, "ssh-connection");
        wr_cstr(&w, "publickey");
        wr_u8(&w, 1u);
        wr_cstr(&w, "ssh-ed25519");
        wr_str(&w, blob, blen);
        wr_str(&w, sig_blob, sb.len);
        if (!w.err) {
            rc = ssh_send_pkt(s, req, (uint32_t)w.len);
            if (rc != SSH_OK) return rc;
            rc = ssh_recv_pkt(s, &pkt_len);
            if (rc != SSH_OK) return rc;
            if (s->pkt[0] == SSH_MSG_USERAUTH_SUCCESS) return SSH_OK;
        }
    }

    /* Try password auth */
    char pw_buf[128];
    const char *pw = cfg->password;
    if (!pw && cfg->password_cb) {
        if (cfg->password_cb(cfg->password_ctx, cfg->user, cfg->host, pw_buf, sizeof(pw_buf))) {
            pw = pw_buf;
        }
    }
    if (pw) {
        w = (ssh_wr_t){ req, sizeof(req), 0, false };
        wr_u8(&w, SSH_MSG_USERAUTH_REQUEST);
        wr_cstr(&w, cfg->user);
        wr_cstr(&w, "ssh-connection");
        wr_cstr(&w, "password");
        wr_u8(&w, 0u);
        wr_cstr(&w, pw);
        crypto_wipe(pw_buf, sizeof(pw_buf));
        if (w.err) return SSH_ERR_NOMEM;
        rc = ssh_send_pkt(s, req, (uint32_t)w.len);
        if (rc != SSH_OK) return rc;
        rc = ssh_recv_pkt(s, &pkt_len);
        if (rc != SSH_OK) return rc;
        if (s->pkt[0] == SSH_MSG_USERAUTH_SUCCESS) return SSH_OK;
    }

    return SSH_ERR_AUTH;
}

/* --- Public Session API ----------------------------------------------------- */

int ssh_open_io(ssh_session_t **out, const ssh_cfg_t *cfg,
                ssh_recv_fn recv_fn, ssh_send_fn send_fn, void *io_ctx) {
    if (!out || !cfg || !cfg->host || !cfg->user || !recv_fn || !send_fn) return SSH_ERR_INVAL;
    ssh_session_t *s = ssh_alloc_session();
    if (!s) return SSH_ERR_NOMEM;
    s->recv = recv_fn;
    s->send = send_fn;
    s->io = io_ctx;
    s->timeout_ms = cfg->timeout_ms ? cfg->timeout_ms : 15000u;
    s->exit_status = -1;

    int rc = ssh_kex(s, cfg);
    if (rc == SSH_OK) rc = ssh_userauth(s, cfg);
    if (rc != SSH_OK) {
        ssh_free_session(s);
        return rc;
    }
    *out = s;
    return SSH_OK;
}

#if !defined(LUGALOS_HOST_TEST)
static int ssh_tcp_recv(void *ctx, uint8_t *buf, uint32_t len) {
    tcp_stream_t *st = (tcp_stream_t *)ctx;
    int n = tcp_stream_read(st, buf, len);
    if (n > 0) return n;
    if (n < 0 || tcp_stream_peer_closed(st)) return -1;
    return 0;
}

static int ssh_tcp_send(void *ctx, const uint8_t *buf, uint32_t len) {
    return tcp_stream_write((tcp_stream_t *)ctx, buf, len);
}

int ssh_connect_tcp(ssh_session_t **out, const ssh_cfg_t *cfg,
                    char *err_out, size_t err_sz) {
    if (!out || !cfg || !cfg->host || !cfg->user) return SSH_ERR_INVAL;
    uint8_t ip[IPV4_LEN];
    if (!dns_resolve(cfg->host, ip)) {
        if (err_out && err_sz) ksnprintf(err_out, err_sz, "DNS failed for %s", cfg->host);
        return SSH_ERR_NET;
    }
    ssh_session_t *s = ssh_alloc_session();
    if (!s) return SSH_ERR_NOMEM;
    s->owns_tcp = true;
    s->timeout_ms = cfg->timeout_ms ? cfg->timeout_ms : 15000u;
    s->exit_status = -1;
    uint16_t port = cfg->port ? cfg->port : SSH_DEFAULT_PORT;
    if (tcp_stream_open(&s->tcp, ip, port) != 0) {
        if (err_out && err_sz) ksnprintf(err_out, err_sz, "TCP open failed");
        ssh_free_session(s);
        return SSH_ERR_NET;
    }
    uint64_t deadline = time_get_ms() + s->timeout_ms;
    int ready = 0;
    while ((ready = tcp_stream_ready(&s->tcp)) == 0) {
        if (console_interrupt_requested() || time_get_ms() >= deadline) {
            tcp_stream_close(&s->tcp);
            ssh_free_session(s);
            if (err_out && err_sz) ksnprintf(err_out, err_sz, "TCP connect timeout");
            return SSH_ERR_TIMEOUT;
        }
        sched_yield();
    }
    if (ready < 0) {
        tcp_stream_close(&s->tcp);
        ssh_free_session(s);
        if (err_out && err_sz) ksnprintf(err_out, err_sz, "Connection refused");
        return SSH_ERR_NET;
    }
    s->recv = ssh_tcp_recv;
    s->send = ssh_tcp_send;
    s->io = &s->tcp;

    int rc = ssh_kex(s, cfg);
    if (rc == SSH_OK) rc = ssh_userauth(s, cfg);
    if (rc != SSH_OK) {
        if (err_out && err_sz) {
            ksnprintf(err_out, err_sz,
                      rc == SSH_ERR_AUTH ? "Authentication failed" :
                      rc == SSH_ERR_HOSTKEY ? "Host key rejected" : "SSH handshake failed (%d)", rc);
        }
        tcp_stream_close(&s->tcp);
        ssh_free_session(s);
        return rc;
    }
    *out = s;
    return SSH_OK;
}
#endif

static int open_session_channel(ssh_session_t *s) {
    uint8_t msg[64];
    ssh_wr_t w = { msg, sizeof(msg), 0, false };
    wr_u8(&w, SSH_MSG_CHANNEL_OPEN);
    wr_cstr(&w, "session");
    wr_u32(&w, 0u);
    wr_u32(&w, SSH_LOCAL_WIN);
    wr_u32(&w, SSH_LOCAL_MAXPKT);
    int rc = ssh_send_pkt(s, msg, (uint32_t)w.len);
    if (rc != SSH_OK) return rc;

    uint32_t pkt_len = 0;
    rc = ssh_recv_pkt(s, &pkt_len);
    if (rc != SSH_OK) return rc;
    if (s->pkt[0] != SSH_MSG_CHANNEL_OPEN_CONF) return SSH_ERR_CHANNEL;
    ssh_span_t sp = { s->pkt + 1u, pkt_len - 1u };
    uint32_t lc = 0;
    if (!rd_u32(&sp, &lc) || !rd_u32(&sp, &s->remote_chan) ||
        !rd_u32(&sp, &s->remote_win) || !rd_u32(&sp, &s->remote_maxpkt)) {
        return SSH_ERR_PROTO;
    }
    s->local_chan = lc;
    s->local_win_avail = SSH_LOCAL_WIN;
    s->chan_open = true;
    return SSH_OK;
}

static int wait_channel_reply(ssh_session_t *s) {
    for (;;) {
        uint32_t pkt_len = 0;
        int rc = ssh_recv_pkt(s, &pkt_len);
        if (rc != SSH_OK) return rc;
        uint8_t t = s->pkt[0];
        if (t == SSH_MSG_CHANNEL_SUCCESS) return SSH_OK;
        if (t == SSH_MSG_CHANNEL_FAILURE) return SSH_ERR_CHANNEL;
        if (t == SSH_MSG_CHANNEL_WIN_ADJ) {
            ssh_span_t sp = { s->pkt + 1u, pkt_len - 1u };
            uint32_t ch, add;
            if (rd_u32(&sp, &ch) && rd_u32(&sp, &add)) s->remote_win += add;
        }
    }
}

int ssh_open_shell(ssh_session_t *s, uint32_t cols, uint32_t rows) {
    if (!s) return SSH_ERR_INVAL;
    int rc = open_session_channel(s);
    if (rc != SSH_OK) return rc;

    uint8_t msg[128];
    ssh_wr_t w = { msg, sizeof(msg), 0, false };
    wr_u8(&w, SSH_MSG_CHANNEL_REQUEST);
    wr_u32(&w, s->remote_chan);
    wr_cstr(&w, "pty-req");
    wr_u8(&w, 1u);
    wr_cstr(&w, "xterm-256color");
    wr_u32(&w, cols ? cols : 80u);
    wr_u32(&w, rows ? rows : 24u);
    wr_u32(&w, 0u);
    wr_u32(&w, 0u);
    wr_cstr(&w, "");
    rc = ssh_send_pkt(s, msg, (uint32_t)w.len);
    if (rc != SSH_OK) return rc;
    rc = wait_channel_reply(s);
    if (rc != SSH_OK) return rc;

    w = (ssh_wr_t){ msg, sizeof(msg), 0, false };
    wr_u8(&w, SSH_MSG_CHANNEL_REQUEST);
    wr_u32(&w, s->remote_chan);
    wr_cstr(&w, "shell");
    wr_u8(&w, 1u);
    rc = ssh_send_pkt(s, msg, (uint32_t)w.len);
    if (rc != SSH_OK) return rc;
    return wait_channel_reply(s);
}

int ssh_open_exec(ssh_session_t *s, const char *command) {
    if (!s || !command) return SSH_ERR_INVAL;
    int rc = open_session_channel(s);
    if (rc != SSH_OK) return rc;

    uint8_t msg[1024];
    ssh_wr_t w = { msg, sizeof(msg), 0, false };
    wr_u8(&w, SSH_MSG_CHANNEL_REQUEST);
    wr_u32(&w, s->remote_chan);
    wr_cstr(&w, "exec");
    wr_u8(&w, 1u);
    wr_cstr(&w, command);
    if (w.err) return SSH_ERR_NOMEM;
    rc = ssh_send_pkt(s, msg, (uint32_t)w.len);
    if (rc != SSH_OK) return rc;
    return wait_channel_reply(s);
}

int ssh_channel_write(ssh_session_t *s, const uint8_t *buf, uint32_t len) {
    if (!s || !s->chan_open) return SSH_ERR_INVAL;
    if (len == 0) return 0;
    if (s->remote_win == 0) return 0;
    uint32_t n = len;
    if (n > s->remote_win) n = s->remote_win;
    if (n > s->remote_maxpkt) n = s->remote_maxpkt;
    if (n > 4096u) n = 4096u;

    ssh_wr_t w = { s->chan_tx, sizeof(s->chan_tx), 0, false };
    wr_u8(&w, SSH_MSG_CHANNEL_DATA);
    wr_u32(&w, s->remote_chan);
    wr_str(&w, buf, n);
    if (w.err) return SSH_ERR_NOMEM;
    int rc = ssh_send_pkt(s, s->chan_tx, (uint32_t)w.len);
    if (rc != SSH_OK) return rc;
    s->remote_win -= n;
    return (int)n;
}

int ssh_channel_read(ssh_session_t *s, uint8_t *buf, uint32_t cap, bool *is_stderr) {
    if (!s || !buf || cap == 0) return SSH_ERR_INVAL;
    if (s->chan_off < s->chan_len) {
        uint32_t avail = s->chan_len - s->chan_off;
        uint32_t n = avail < cap ? avail : cap;
        memcpy(buf, s->chan_buf + s->chan_off, n);
        s->chan_off += n;
        if (is_stderr) *is_stderr = s->chan_is_stderr;
        return (int)n;
    }
    if (s->chan_eof || s->chan_closed) return SSH_EOF;

    /* Non-blocking check: pull 1 byte before blocking on full packet. */
    uint8_t peek = 0;
    int n1 = s->recv(s->io, &peek, 1u);
    if (n1 < 0) return SSH_EOF;
    if (n1 == 0) return 0;

    uint8_t hdr_enc[4];
    hdr_enc[0] = peek;
    int rc = io_recv_exact(s, hdr_enc + 1u, 3u);
    if (rc != SSH_OK) return rc;

    uint32_t len = 0;
    rc = ssh_finish_pkt(s, hdr_enc, &len);
    if (rc != SSH_OK) return rc;
    if (len == 0) return 0;

    uint8_t t = s->pkt[0];
    ssh_span_t sp = { s->pkt + 1u, len - 1u };
    if (t == SSH_MSG_CHANNEL_DATA || t == SSH_MSG_CHANNEL_EXT_DATA) {
        uint32_t ch = 0, dtype = 0;
        ssh_span_t data;
        if (!rd_u32(&sp, &ch)) return SSH_ERR_PROTO;
        if (t == SSH_MSG_CHANNEL_EXT_DATA && !rd_u32(&sp, &dtype)) return SSH_ERR_PROTO;
        if (!rd_str(&sp, &data)) return SSH_ERR_PROTO;
        if (data.n > sizeof(s->chan_buf)) return SSH_ERR_NOMEM;
        memcpy(s->chan_buf, data.p, data.n);
        s->chan_off = 0;
        s->chan_len = (uint32_t)data.n;
        s->chan_is_stderr = (t == SSH_MSG_CHANNEL_EXT_DATA);
        if (s->local_win_avail >= (uint32_t)data.n) s->local_win_avail -= (uint32_t)data.n;
        else s->local_win_avail = 0;
        if (s->local_win_avail < SSH_LOCAL_WIN / 2u) {
            uint8_t adj[9];
            ssh_wr_t aw = { adj, sizeof(adj), 0, false };
            wr_u8(&aw, SSH_MSG_CHANNEL_WIN_ADJ);
            wr_u32(&aw, s->remote_chan);
            wr_u32(&aw, SSH_LOCAL_WIN - s->local_win_avail);
            if (ssh_send_pkt(s, adj, (uint32_t)aw.len) == SSH_OK) {
                s->local_win_avail = SSH_LOCAL_WIN;
            }
        }
        uint32_t n = s->chan_len < cap ? s->chan_len : cap;
        memcpy(buf, s->chan_buf, n);
        s->chan_off = n;
        if (is_stderr) *is_stderr = s->chan_is_stderr;
        return (int)n;
    }
    if (t == SSH_MSG_CHANNEL_WIN_ADJ) {
        uint32_t ch, add;
        if (rd_u32(&sp, &ch) && rd_u32(&sp, &add)) s->remote_win += add;
        return 0;
    }
    if (t == SSH_MSG_CHANNEL_REQUEST) {
        uint32_t ch;
        ssh_span_t rname;
        uint8_t want_reply = 0;
        if (rd_u32(&sp, &ch) && rd_str(&sp, &rname) && rd_u8(&sp, &want_reply)) {
            if (rname.n == 11u && memcmp(rname.p, "exit-status", 11u) == 0) {
                uint32_t st = 0;
                if (rd_u32(&sp, &st)) s->exit_status = (int)st;
            }
            if (want_reply) {
                uint8_t rep[5];
                rep[0] = SSH_MSG_CHANNEL_FAILURE;
                wr_be32(rep + 1u, s->remote_chan);
                (void)ssh_send_pkt(s, rep, sizeof(rep));
            }
        }
        return 0;
    }
    if (t == SSH_MSG_GLOBAL_REQUEST) {
        ssh_span_t rname;
        uint8_t want_reply = 0;
        if (rd_str(&sp, &rname) && rd_u8(&sp, &want_reply) && want_reply) {
            uint8_t rep[1] = { SSH_MSG_REQUEST_FAILURE };
            (void)ssh_send_pkt(s, rep, sizeof(rep));
        }
        return 0;
    }
    if (t == SSH_MSG_CHANNEL_EOF)   { s->chan_eof = true; return SSH_EOF; }
    if (t == SSH_MSG_CHANNEL_CLOSE) { s->chan_closed = true; return SSH_EOF; }
    if (t == SSH_MSG_DISCONNECT)    { s->chan_closed = true; return SSH_EOF; }
    return 0;
}

int ssh_channel_window_change(ssh_session_t *s, uint32_t cols, uint32_t rows) {
    if (!s || !s->chan_open) return SSH_ERR_INVAL;
    uint8_t msg[64];
    ssh_wr_t w = { msg, sizeof(msg), 0, false };
    wr_u8(&w, SSH_MSG_CHANNEL_REQUEST);
    wr_u32(&w, s->remote_chan);
    wr_cstr(&w, "window-change");
    wr_u8(&w, 0u);
    wr_u32(&w, cols);
    wr_u32(&w, rows);
    wr_u32(&w, 0u);
    wr_u32(&w, 0u);
    return ssh_send_pkt(s, msg, (uint32_t)w.len);
}

int ssh_channel_send_eof(ssh_session_t *s) {
    if (!s || !s->chan_open) return SSH_ERR_INVAL;
    uint8_t msg[5];
    msg[0] = SSH_MSG_CHANNEL_EOF;
    wr_be32(msg + 1u, s->remote_chan);
    return ssh_send_pkt(s, msg, sizeof(msg));
}

int ssh_channel_exit_status(const ssh_session_t *s) {
    return s ? s->exit_status : -1;
}

void ssh_close(ssh_session_t *s) {
    if (!s) return;
    if (s->chan_open && !s->chan_closed) {
        uint8_t msg[5];
        msg[0] = SSH_MSG_CHANNEL_CLOSE;
        wr_be32(msg + 1u, s->remote_chan);
        (void)ssh_send_pkt(s, msg, sizeof(msg));
    }
#if !defined(LUGALOS_HOST_TEST)
    if (s->owns_tcp) tcp_stream_close(&s->tcp);
#endif
    ssh_free_session(s);
}

#else
typedef int ssh_unused_t;
#endif
