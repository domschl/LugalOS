/* The node's HTTPS key, its certificate request and its certificate, as /dev files (kernel/tls_cert.h). */

#include "lugalos_config.h"
#include "kernel/tls_cert.h"
#include "kernel/der.h"
#include "kernel/crypto.h"
#include "kernel/sha256.h"
#include "kernel/identity.h"
#include "kernel/lock.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/scratch.h"
#include "net/ip.h"
#include "net/httpd.h"
#include <string.h>

#define CSR_DER_MAX 1024u

/* The request as first read, kept so a client reading it in parts gets one request (a new signature
 * would differ), and the certificate being written, until it is complete. One page each, taken when
 * first needed. */
typedef struct {
    uint32_t der_len, pem_len;
    uint8_t  der[CSR_DER_MAX];
    char     pem[4096 - 8 - CSR_DER_MAX];
} csr_cache_t;
typedef struct {
    uint32_t len;
    char     text[4096 - 4];
} cert_stage_t;
_Static_assert(sizeof(csr_cache_t) <= 4096, "one page");
_Static_assert(sizeof(cert_stage_t) <= 4096, "one page");

/* guarded by: g_lock -- the shell, the 9P server and the web server reach these files. */
static ylock_t g_lock;
static csr_cache_t  *g_csr;
static cert_stage_t *g_stage;
/* guarded by: g_lock. After an install, the rest of the chain the client is still writing is taken
 * and ignored: g_tail_off is where its next write lands. */
static uint32_t g_tail_off;
static bool     g_tail_ok;

const char *tls_result_str(int rc) {
    switch (rc) {
    case TLS_OK:            return "ok";
    case TLS_ERR_NO_KEY:    return "no key (keygen first)";
    case TLS_ERR_HAVE_KEY:  return "there is a key already (rekey replaces it, and its certificate)";
    case TLS_ERR_ENTROPY:   return "no hardware entropy: no key and no signature on this boot";
    case TLS_ERR_STORE:     return "the identity record could not be written";
    case TLS_ERR_CERT:      return "not a P-256 certificate";
    case TLS_ERR_WRONG_KEY: return "that certificate is for another key";
    case TLS_ERR_MEMORY:    return "out of memory";
    default:                return "error";
    }
}

/* Called with g_lock held. */
static void forget_csr(void) {
    if (g_csr) { palloc_free(g_csr, 1); g_csr = NULL; }
}

int tls_keygen(bool replace) {
    uint8_t key[NODE_TLS_KEY_LEN];
    ylock_acquire(&g_lock);
    int rc = TLS_OK;
    if (!replace && node_tls_key(key)) {
        rc = TLS_ERR_HAVE_KEY;
    } else {
        int c = p256_keypair(key, key + 32);
        if (c == CRYPTO_ERR_NO_ENTROPY) rc = TLS_ERR_ENTROPY;
        else if (c != CRYPTO_OK) rc = TLS_ERR_NO_KEY;
        else if (node_identity_set_tls_key(key) != NODE_ID_OK) rc = TLS_ERR_STORE;
        else forget_csr();
    }
    crypto_wipe_bytes(key, sizeof key);
    ylock_release(&g_lock);
    if (rc == TLS_OK) { printk("[TLS] a new key (the certificate, if any, went with the old one)\n"); httpd_tls_changed(); }
    return rc;
}

int tls_clear(void) {
    ylock_acquire(&g_lock);
    int rc = node_identity_clear_tls() == NODE_ID_OK ? TLS_OK : TLS_ERR_STORE;
    if (rc == TLS_OK) forget_csr();
    ylock_release(&g_lock);
    if (rc == TLS_OK) httpd_tls_changed();
    return rc;
}

static void hex8(const uint8_t *b, char out[17]) {
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) { out[2 * i] = d[b[i] >> 4]; out[2 * i + 1] = d[b[i] & 15]; }
    out[16] = 0;
}

/* The node's names for a request: <name>.local, and its address when it has one. */
static void csr_names(char cn[64], uint8_t ip[4], bool *have_ip) {
    ksnprintf(cn, 64, "%s.local", node_name());
    const net_state_t *st = net_state();
    *have_ip = net_configured() && (st->ip[0] | st->ip[1] | st->ip[2] | st->ip[3]) != 0;
    if (*have_ip) memcpy(ip, st->ip, 4);
}

uint32_t tls_ctl_read(char *out, uint32_t cap) {
    uint8_t key[NODE_TLS_KEY_LEN];
    uint32_t n = 0;
    bool have_key = node_tls_key(key);
    if (have_key) {
        uint8_t h[SHA256_DIGEST_LEN];
        char fp[17];
        sha256(key + 32, 64, h);
        hex8(h, fp);
        n += (uint32_t)ksnprintf(out + n, cap - n, "key=p256\nkey_fingerprint=%s\n", fp);
    } else {
        n += (uint32_t)ksnprintf(out + n, cap - n, "key=none\n");
    }
    scratch_t sc;
    if (scratch_acquire(&sc, NODE_TLS_CERT_MAX + sizeof(tls_cert_info_t))) {
        uint8_t *der = (uint8_t *)sc.base;
        tls_cert_info_t *ci = (tls_cert_info_t *)(der + NODE_TLS_CERT_MAX);
        uint32_t len = 0;
        if (!node_tls_cert(der, NODE_TLS_CERT_MAX, &len)) {
            n += (uint32_t)ksnprintf(out + n, cap - n, "cert=none\n");
        } else if (tls_cert_parse(der, len, ci) != TLS_OK) {
            n += (uint32_t)ksnprintf(out + n, cap - n, "cert=unreadable\n");
        } else {
            bool match = have_key && crypto_equal(ci->pub, key + 32, 64);
            n += (uint32_t)ksnprintf(out + n, cap - n,
                                     "cert=installed\nsubject=%s\nissuer=%s\nnot_before=%s\nnot_after=%s\nmatches_key=%s\n",
                                     ci->subject_cn, ci->issuer_cn, ci->not_before, ci->not_after,
                                     match ? "yes" : "no");
        }
        scratch_release(&sc);
    }
    char cn[64];
    uint8_t ip[4];
    bool have_ip;
    csr_names(cn, ip, &have_ip);
    if (have_ip) n += (uint32_t)ksnprintf(out + n, cap - n, "names=%s %u.%u.%u.%u\n", cn, ip[0], ip[1], ip[2], ip[3]);
    else n += (uint32_t)ksnprintf(out + n, cap - n, "names=%s\n", cn);
    crypto_wipe_bytes(key, sizeof key);
    return n < cap ? n : cap;
}

int tls_ctl_write(const char *buf, uint32_t count) {
    uint32_t len = count;
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) len--;
    int rc;
    if (len == 6 && memcmp(buf, "keygen", 6) == 0) rc = tls_keygen(false);
    else if (len == 5 && memcmp(buf, "rekey", 5) == 0) rc = tls_keygen(true);
    else if (len == 5 && memcmp(buf, "clear", 5) == 0) rc = tls_clear();
    else return -1;
    if (rc != TLS_OK) printk("[TLS] %.*s: %s\n", (int)len, buf, tls_result_str(rc));
    return rc == TLS_OK ? (int)count : -1;
}

/* Builds the request into g_csr. Called with g_lock held. */
static int build_csr(void) {
    uint8_t key[NODE_TLS_KEY_LEN];
    if (!node_tls_key(key)) return TLS_ERR_NO_KEY;
    csr_cache_t *c = (csr_cache_t *)palloc_pages(1);
    if (!c) { crypto_wipe_bytes(key, sizeof key); return TLS_ERR_MEMORY; }
    char cn[64];
    uint8_t ip[4];
    bool have_ip;
    csr_names(cn, ip, &have_ip);
    int n = tls_csr_build(key, key + 32, cn, have_ip ? ip : NULL, c->der, sizeof c->der);
    crypto_wipe_bytes(key, sizeof key);
    if (n < 0) { palloc_free(c, 1); return n; }
    c->der_len = (uint32_t)n;
    c->pem_len = pem_encode("CERTIFICATE REQUEST", c->der, c->der_len, c->pem, sizeof c->pem);
    if (c->pem_len == 0) { palloc_free(c, 1); return TLS_ERR_MEMORY; }
    g_csr = c;
    return TLS_OK;
}

static int read_from(const char *src, uint32_t len, void *buf, uint32_t count, uint64_t off) {
    if (off >= len) return 0;
    uint32_t k = len - (uint32_t)off;
    if (k > count) k = count;
    memcpy(buf, src + off, k);
    return (int)k;
}

int tls_csr_read(void *buf, uint32_t count, uint64_t off) {
    ylock_acquire(&g_lock);
    int rc = TLS_OK;
    if (!g_csr) rc = build_csr();
    int n = rc == TLS_OK ? read_from(g_csr->pem, g_csr->pem_len, buf, count, off) : -1;
    ylock_release(&g_lock);
    if (rc != TLS_OK && off == 0) printk("[TLS] no certificate request: %s\n", tls_result_str(rc));
    return n;
}

int tls_certfile_read(void *buf, uint32_t count, uint64_t off) {
    scratch_t sc;
    if (!scratch_acquire(&sc, 4096)) return -1;
    uint8_t *der = (uint8_t *)sc.base;
    char *pem = (char *)sc.base + NODE_TLS_CERT_MAX;
    uint32_t len = 0;
    int n = 0;
    if (node_tls_cert(der, NODE_TLS_CERT_MAX, &len)) {
        uint32_t pl = pem_encode("CERTIFICATE", der, len, pem, 4096 - NODE_TLS_CERT_MAX);
        n = pl ? read_from(pem, pl, buf, count, off) : -1;
    }
    scratch_release(&sc);
    return n;
}

/* Installs the first complete certificate block in g_stage, if there is one. Called with g_lock
 * held. Returns TLS_OK when one was installed, 1 while the block is incomplete, or a TLS_ERR_*. */
static int try_install(void) {
    cert_stage_t *s = g_stage;
    uint8_t *der = (uint8_t *)palloc_pages(1);
    if (!der) return TLS_ERR_MEMORY;
    int rc;
    int n = pem_decode(s->text, s->len, "CERTIFICATE", der, NODE_TLS_CERT_MAX, NULL);
    if (n < 0) {
        /* No complete block yet -- or a broken one, which only the END line tells apart. */
        static const char END[] = "-----END CERTIFICATE-----";
        bool ended = false;
        for (uint32_t i = 0; i + sizeof END - 1 <= s->len && !ended; i++) ended = memcmp(s->text + i, END, sizeof END - 1) == 0;
        rc = ended ? TLS_ERR_CERT : 1;
    } else {
        tls_cert_info_t ci;
        uint8_t key[NODE_TLS_KEY_LEN];
        if (tls_cert_parse(der, (uint32_t)n, &ci) != TLS_OK) rc = TLS_ERR_CERT;
        else if (!node_tls_key(key)) rc = TLS_ERR_NO_KEY;
        else if (!crypto_equal(ci.pub, key + 32, 64)) rc = TLS_ERR_WRONG_KEY;
        else if (node_identity_set_tls_cert(der, (uint32_t)n) != NODE_ID_OK) rc = TLS_ERR_STORE;
        else { rc = TLS_OK; forget_csr(); httpd_tls_changed(); printk("[TLS] certificate installed: %s, from %s, until %s\n",
                                                  ci.subject_cn, ci.issuer_cn, ci.not_after); }
        crypto_wipe_bytes(key, sizeof key);
    }
    palloc_free(der, 1);
    return rc;
}

int tls_certfile_write(const void *buf, uint32_t count, uint64_t off) {
    ylock_acquire(&g_lock);
    int ret = -1;
    if (off == 0) {
        g_tail_ok = false;
        if (!g_stage) g_stage = (cert_stage_t *)palloc_pages(1);
        if (g_stage) g_stage->len = 0;
    }
    cert_stage_t *s = g_stage;
    if (!s && g_tail_ok && off == g_tail_off) {
        g_tail_off += count;
        ret = (int)count;
    } else if (s && off == s->len && s->len + count <= sizeof s->text) {
        memcpy(s->text + s->len, buf, count);
        s->len += count;
        int rc = try_install();
        if (rc == 1) {
            ret = (int)count;                    /* not complete yet */
        } else {
            if (rc == TLS_OK) { ret = (int)count; g_tail_ok = true; g_tail_off = s->len; }
            else printk("[TLS] certificate refused: %s\n", tls_result_str(rc));
            palloc_free(s, 1);
            g_stage = NULL;
        }
    }
    ylock_release(&g_lock);
    return ret;
}
