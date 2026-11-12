#define _POSIX_C_SOURCE 200809L
#include "net/ssh.h"
#include "kernel/crypto.h"
#include "kernel/sha256.h"
#include "third_party/crypto/monocypher/monocypher.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

bool random_is_hardware(void) { return true; }
uint64_t time_get_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static int sock_recv(void *ctx, uint8_t *buf, uint32_t len) {
    int fd = *(int *)ctx;
    ssize_t n = recv(fd, buf, len, MSG_DONTWAIT);
    if (n > 0) return (int)n;
    if (n == 0) return -1;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}

static int sock_send(void *ctx, const uint8_t *buf, uint32_t len) {
    int fd = *(int *)ctx;
    ssize_t n = send(fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n > 0) return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;
}

/* --- Minimal in-process SSH-2 server for ASan/UBSan round-trip testing ------ */

static void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void srv_send_exact(int fd, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        assert(n > 0);
        off += (size_t)n;
    }
}

static void srv_recv_exact(int fd, uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, buf + off, len - off);
        assert(n > 0);
        off += (size_t)n;
    }
}

struct srv_ctx {
    int fd;
    bool require_password;
    bool reject_all;
    bool tamper_mac;
};

static void srv_send_pkt(int fd, bool enc, const uint8_t key[64], uint32_t *seq,
                         const uint8_t *payload, uint32_t plen) {
    uint32_t pad = 8u - ((1u + plen) & 7u);
    if (pad < 4u) pad += 8u;
    uint32_t pkt_len = 1u + plen + pad;
    uint8_t plain[4096], tx[4160];
    plain[0] = (uint8_t)pad;
    if (plen) memcpy(plain + 1u, payload, plen);
    memset(plain + 1u + plen, 0x42, pad);
    if (!enc) {
        wr_be32(tx, pkt_len);
        memcpy(tx + 4u, plain, pkt_len);
        (*seq)++;
        srv_send_exact(fd, tx, 4u + pkt_len);
        return;
    }
    uint8_t nonce[12] = {0};
    wr_be32(nonce + 8u, *seq);
    uint8_t lbe[4];
    wr_be32(lbe, pkt_len);
    crypto_chacha20_ietf(tx, lbe, 4u, key + 32u, nonce, 0);
    uint8_t pk[64] = {0};
    crypto_chacha20_ietf(pk, pk, 64u, key, nonce, 0);
    crypto_chacha20_ietf(tx + 4u, plain, pkt_len, key, nonce, 1);
    crypto_poly1305_ctx pctx;
    crypto_poly1305_init(&pctx, pk);
    crypto_poly1305_update(&pctx, tx, 4u + pkt_len);
    crypto_poly1305_final(&pctx, tx + 4u + pkt_len);
    (*seq)++;
    srv_send_exact(fd, tx, 4u + pkt_len + 16u);
}

static void srv_send_bad_mac(int fd, const uint8_t key[64], uint32_t *seq,
                             const uint8_t *payload, uint32_t len) {
    uint8_t plain[4096], tx[4160];
    uint32_t raw = 1u + len;
    uint32_t pad = 8u - (raw % 8u);
    if (pad < 4u) pad += 8u;
    uint32_t pkt_len = raw + pad;
    plain[0] = (uint8_t)pad;
    memcpy(plain + 1u, payload, len);
    memset(plain + 1u + len, 0xaa, pad);
    uint8_t nonce[12] = {0}, lbe[4], pk[64] = {0};
    wr_be32(nonce + 8u, *seq);
    wr_be32(lbe, pkt_len);
    crypto_chacha20_ietf(tx, lbe, 4u, key + 32u, nonce, 0);
    crypto_chacha20_ietf(pk, pk, 64u, key, nonce, 0);
    crypto_chacha20_ietf(tx + 4u, plain, pkt_len, key, nonce, 1);
    crypto_poly1305_ctx pctx;
    crypto_poly1305_init(&pctx, pk);
    crypto_poly1305_update(&pctx, tx, 4u + pkt_len);
    crypto_poly1305_final(&pctx, tx + 4u + pkt_len);
    tx[4u + pkt_len] ^= 0xffu; /* Corrupt Poly1305 tag */
    (*seq)++;
    srv_send_exact(fd, tx, 4u + pkt_len + 16u);
}

static uint32_t srv_recv_pkt(int fd, bool enc, const uint8_t key[64], uint32_t *seq,
                             uint8_t *out_payload) {
    uint8_t hdr[4], ct[4160], plain[4096];
    srv_recv_exact(fd, hdr, 4u);
    uint32_t pkt_len = 0;
    if (!enc) {
        pkt_len = rd_be32(hdr);
        srv_recv_exact(fd, plain, pkt_len);
        (*seq)++;
    } else {
        uint8_t nonce[12] = {0}, lplain[4];
        wr_be32(nonce + 8u, *seq);
        crypto_chacha20_ietf(lplain, hdr, 4u, key + 32u, nonce, 0);
        pkt_len = rd_be32(lplain);
        srv_recv_exact(fd, ct, pkt_len + 16u);
        uint8_t pk[64] = {0}, tag[16];
        crypto_chacha20_ietf(pk, pk, 64u, key, nonce, 0);
        crypto_poly1305_ctx pctx;
        crypto_poly1305_init(&pctx, pk);
        crypto_poly1305_update(&pctx, hdr, 4u);
        crypto_poly1305_update(&pctx, ct, pkt_len);
        crypto_poly1305_final(&pctx, tag);
        assert(crypto_verify16(tag, ct + pkt_len) == 0);
        crypto_chacha20_ietf(plain, ct, pkt_len, key, nonce, 1);
        (*seq)++;
    }
    uint8_t pad = plain[0];
    uint32_t plen = pkt_len - 1u - pad;
    memcpy(out_payload, plain + 1u, plen);
    return plen;
}

static size_t put_str(uint8_t *dst, const void *p, size_t n) {
    wr_be32(dst, (uint32_t)n);
    if (n) memcpy(dst + 4u, p, n);
    return 4u + n;
}

static void sha_str(sha256_ctx_t *h, const void *p, size_t n) {
    uint8_t l[4];
    wr_be32(l, (uint32_t)n);
    sha256_update(h, l, 4u);
    if (n) sha256_update(h, p, n);
}

static void sha_mpint(sha256_ctx_t *h, const uint8_t *p, size_t n) {
    while (n > 0 && *p == 0) { p++; n--; }
    uint8_t l[4];
    uint8_t pad = (n > 0 && (p[0] & 0x80u)) ? 1u : 0u;
    wr_be32(l, (uint32_t)(n + pad));
    sha256_update(h, l, 4u);
    if (pad) { uint8_t z = 0; sha256_update(h, &z, 1u); }
    if (n) sha256_update(h, p, n);
}

static void srv_derive(const uint8_t k[32], const uint8_t h[32], char ch, uint8_t out[64]) {
    sha256_ctx_t c;
    sha256_init(&c);
    sha_mpint(&c, k, 32u);
    sha256_update(&c, h, 32u);
    sha256_update(&c, (const uint8_t *)&ch, 1u);
    sha256_update(&c, h, 32u);
    sha256_final(&c, out);
    sha256_init(&c);
    sha_mpint(&c, k, 32u);
    sha256_update(&c, h, 32u);
    sha256_update(&c, out, 32u);
    sha256_final(&c, out + 32u);
}

static void *ssh_test_server(void *arg) {
    struct srv_ctx *sc = (struct srv_ctx *)arg;
    int fd = sc->fd;

    const char *sban = "SSH-2.0-TestSrv_1.0\r\n";
    srv_send_exact(fd, (const uint8_t *)sban, strlen(sban));

    char cban[128];
    size_t bp = 0;
    for (;;) {
        uint8_t ch;
        srv_recv_exact(fd, &ch, 1u);
        if (ch == '\n') break;
        if (ch != '\r' && bp < sizeof(cban) - 1u) cban[bp++] = (char)ch;
    }
    cban[bp] = '\0';

    uint32_t tx_seq = 0, rx_seq = 0;
    uint8_t c2s[64] = {0}, s2c[64] = {0};
    uint8_t kex_c[2048], kex_s[512], pkt[4096];
    uint32_t kex_c_len = srv_recv_pkt(fd, false, NULL, &rx_seq, kex_c);
    assert(kex_c[0] == 20u);

    size_t ks_pos = 0;
    kex_s[ks_pos++] = 20u;
    memset(kex_s + ks_pos, 0x11, 16u); ks_pos += 16u;
    ks_pos += put_str(kex_s + ks_pos, "curve25519-sha256", 17u);
    ks_pos += put_str(kex_s + ks_pos, "ssh-ed25519", 11u);
    ks_pos += put_str(kex_s + ks_pos, "chacha20-poly1305@openssh.com", 29u);
    ks_pos += put_str(kex_s + ks_pos, "chacha20-poly1305@openssh.com", 29u);
    ks_pos += put_str(kex_s + ks_pos, "none", 4u);
    ks_pos += put_str(kex_s + ks_pos, "none", 4u);
    ks_pos += put_str(kex_s + ks_pos, "none", 4u);
    ks_pos += put_str(kex_s + ks_pos, "none", 4u);
    ks_pos += put_str(kex_s + ks_pos, "", 0u);
    ks_pos += put_str(kex_s + ks_pos, "", 0u);
    kex_s[ks_pos++] = 0u;
    wr_be32(kex_s + ks_pos, 0u); ks_pos += 4u;
    srv_send_pkt(fd, false, NULL, &tx_seq, kex_s, (uint32_t)ks_pos);

    uint32_t plen = srv_recv_pkt(fd, false, NULL, &rx_seq, pkt);
    assert(pkt[0] == 30u && plen >= 37u);
    const uint8_t *qc = pkt + 5u;

    static const uint8_t host_seed[32] = { 7, 8, 9, 10 };
    uint8_t host_sec[64], host_pub[32];
    ed25519_keypair_from_seed(host_sec, host_pub, host_seed);
    uint8_t ks_blob[64];
    size_t ks_blen = ssh_encode_ed25519_blob(host_pub, ks_blob, sizeof(ks_blob));

    static const uint8_t srv_eph_priv[32] = { 0x44, 0x55, 0x66 };
    uint8_t srv_eph_pub[32], shared[32];
    crypto_x25519_public_key(srv_eph_pub, srv_eph_priv);
    crypto_x25519(shared, srv_eph_priv, qc);

    uint8_t h[32];
    sha256_ctx_t hc;
    sha256_init(&hc);
    sha_str(&hc, cban, strlen(cban));
    sha_str(&hc, "SSH-2.0-TestSrv_1.0", 19u);
    sha_str(&hc, kex_c, kex_c_len);
    sha_str(&hc, kex_s, ks_pos);
    sha_str(&hc, ks_blob, ks_blen);
    sha_str(&hc, qc, 32u);
    sha_str(&hc, srv_eph_pub, 32u);
    sha_mpint(&hc, shared, 32u);
    sha256_final(&hc, h);

    uint8_t sig[64], sig_blob[96];
    ed25519_sign(sig, host_sec, h, 32u);
    size_t sb = 0;
    sb += put_str(sig_blob + sb, "ssh-ed25519", 11u);
    sb += put_str(sig_blob + sb, sig, 64u);

    uint8_t rep[512];
    size_t rp = 0;
    rep[rp++] = 31u;
    rp += put_str(rep + rp, ks_blob, ks_blen);
    rp += put_str(rep + rp, srv_eph_pub, 32u);
    rp += put_str(rep + rp, sig_blob, sb);
    srv_send_pkt(fd, false, NULL, &tx_seq, rep, (uint32_t)rp);

    uint8_t nk = 21u;
    srv_send_pkt(fd, false, NULL, &tx_seq, &nk, 1u);
    plen = srv_recv_pkt(fd, false, NULL, &rx_seq, pkt);
    if (plen == 0 || pkt[0] != 21u) { close(fd); return NULL; }

    srv_derive(shared, h, 'C', c2s);
    srv_derive(shared, h, 'D', s2c);

    /* Service request */
    plen = srv_recv_pkt(fd, true, c2s, &rx_seq, pkt);
    assert(pkt[0] == 5u);
    uint8_t sa[32];
    sa[0] = 6u;
    size_t sap = 1u + put_str(sa + 1u, "ssh-userauth", 12u);
    srv_send_pkt(fd, true, s2c, &tx_seq, sa, (uint32_t)sap);

    /* Userauth */
    for (;;) {
        plen = srv_recv_pkt(fd, true, c2s, &rx_seq, pkt);
        assert(pkt[0] == 50u);
        if (sc->reject_all) {
            uint8_t fail[32];
            fail[0] = 51u;
            size_t fp = 1u + put_str(fail + 1u, "publickey,password", 18u);
            fail[fp++] = 0u;
            srv_send_pkt(fd, true, s2c, &tx_seq, fail, (uint32_t)fp);
            break;
        }
        uint8_t ok = 52u;
        srv_send_pkt(fd, true, s2c, &tx_seq, &ok, 1u);
        break;
    }
    if (sc->reject_all) { close(fd); return NULL; }

    /* Channel open */
    plen = srv_recv_pkt(fd, true, c2s, &rx_seq, pkt);
    assert(pkt[0] == 90u);
    uint8_t conf[17];
    conf[0] = 91u;
    wr_be32(conf + 1u, 0u);
    wr_be32(conf + 5u, 7u);
    wr_be32(conf + 9u, 65536u);
    wr_be32(conf + 13u, 16384u);
    srv_send_pkt(fd, true, s2c, &tx_seq, conf, sizeof(conf));

    /* Exec request */
    plen = srv_recv_pkt(fd, true, c2s, &rx_seq, pkt);
    assert(pkt[0] == 98u);
    uint8_t csuc[5];
    csuc[0] = 99u; wr_be32(csuc + 1u, 0u);
    srv_send_pkt(fd, true, s2c, &tx_seq, csuc, sizeof(csuc));

    /* Send stdout + exit-status + EOF + CLOSE */
    uint8_t cdata[64];
    cdata[0] = 94u; wr_be32(cdata + 1u, 0u);
    size_t cdp = 5u + put_str(cdata + 5u, "hello from sshd\n", 16u);
    if (sc->tamper_mac) {
        srv_send_bad_mac(fd, s2c, &tx_seq, cdata, (uint32_t)cdp);
        close(fd);
        return NULL;
    }
    srv_send_pkt(fd, true, s2c, &tx_seq, cdata, (uint32_t)cdp);

    uint8_t ex[32];
    ex[0] = 98u; wr_be32(ex + 1u, 0u);
    size_t ep = 5u + put_str(ex + 5u, "exit-status", 11u);
    ex[ep++] = 0u;
    wr_be32(ex + ep, 0u); ep += 4u;
    srv_send_pkt(fd, true, s2c, &tx_seq, ex, (uint32_t)ep);

    uint8_t ceof[5]; ceof[0] = 96u; wr_be32(ceof + 1u, 0u);
    srv_send_pkt(fd, true, s2c, &tx_seq, ceof, sizeof(ceof));
    uint8_t ccls[5]; ccls[0] = 97u; wr_be32(ccls + 1u, 0u);
    srv_send_pkt(fd, true, s2c, &tx_seq, ccls, sizeof(ccls));

    close(fd);
    return NULL;
}

static bool accept_hk(void *ctx, const char *host, uint16_t port,
                      const char *ktype, const uint8_t *blob, size_t len) {
    (void)ctx; (void)host; (void)port; (void)blob; (void)len;
    assert(strcmp(ktype, "ssh-ed25519") == 0);
    return true;
}

int main(void) {
    /* 1. Key formatting & fingerprint */
    static const uint8_t seed[32] = { 1, 2, 3, 4 };
    uint8_t sec[64], pub[32], blob[64];
    ed25519_keypair_from_seed(sec, pub, seed);
    size_t blen = ssh_encode_ed25519_blob(pub, blob, sizeof(blob));
    assert(blen == 51u);
    char pubtxt[160], fp[64];
    assert(ssh_format_ed25519_pub(pub, "test@lugalos", pubtxt, sizeof(pubtxt)) > 0);
    assert(strncmp(pubtxt, "ssh-ed25519 ", 12u) == 0);
    assert(ssh_key_fingerprint_sha256(blob, blen, fp, sizeof(fp)) > 0);
    assert(strncmp(fp, "SHA256:", 7u) == 0);

    /* 2. Full SSH-2 session with Ed25519 pubkey auth + exec */
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    struct srv_ctx sc = { fds[1], false, false, false };
    pthread_t th;
    assert(pthread_create(&th, NULL, ssh_test_server, &sc) == 0);

    ssh_cfg_t cfg = {
        .host = "localhost",
        .port = 22,
        .user = "alice",
        .ed25519_seed = seed,
        .timeout_ms = 5000,
        .hostkey_cb = accept_hk,
    };
    ssh_session_t *s = NULL;
    assert(ssh_open_io(&s, &cfg, sock_recv, sock_send, &fds[0]) == SSH_OK);
    assert(ssh_open_exec(s, "uname -a") == SSH_OK);

    char out[64];
    size_t pos = 0;
    for (;;) {
        bool is_err = false;
        int n = ssh_channel_read(s, (uint8_t *)out + pos, sizeof(out) - 1u - pos, &is_err);
        if (n == SSH_EOF) break;
        assert(n >= 0);
        if (n > 0) pos += (size_t)n;
    }
    out[pos] = '\0';
    assert(strcmp(out, "hello from sshd\n") == 0);
    assert(ssh_channel_exit_status(s) == 0);
    ssh_close(s);
    close(fds[0]);
    pthread_join(th, NULL);

    /* 3. Password authentication */
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    sc = (struct srv_ctx){ fds[1], false, false, false };
    assert(pthread_create(&th, NULL, ssh_test_server, &sc) == 0);
    ssh_cfg_t pw_cfg = {
        .host = "localhost", .port = 22, .user = "bob",
        .password = "s3cr3t", .timeout_ms = 5000, .hostkey_cb = accept_hk,
    };
    s = NULL;
    assert(ssh_open_io(&s, &pw_cfg, sock_recv, sock_send, &fds[0]) == SSH_OK);
    assert(ssh_open_exec(s, "id") == SSH_OK);
    pos = 0;
    for (;;) {
        bool is_err = false;
        int n = ssh_channel_read(s, (uint8_t *)out + pos, sizeof(out) - 1u - pos, &is_err);
        if (n == SSH_EOF) break;
        assert(n >= 0);
        if (n > 0) pos += (size_t)n;
    }
    out[pos] = '\0';
    assert(strcmp(out, "hello from sshd\n") == 0);
    ssh_close(s);
    close(fds[0]);
    pthread_join(th, NULL);

    /* 4. Auth rejection */
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    sc = (struct srv_ctx){ fds[1], false, true, false };
    assert(pthread_create(&th, NULL, ssh_test_server, &sc) == 0);
    s = NULL;
    assert(ssh_open_io(&s, &pw_cfg, sock_recv, sock_send, &fds[0]) == SSH_ERR_AUTH);
    close(fds[0]);
    pthread_join(th, NULL);

    /* 5. Tampered Poly1305 MAC rejected */
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    sc = (struct srv_ctx){ fds[1], false, false, true };
    assert(pthread_create(&th, NULL, ssh_test_server, &sc) == 0);
    s = NULL;
    assert(ssh_open_io(&s, &cfg, sock_recv, sock_send, &fds[0]) == SSH_OK);
    assert(ssh_open_exec(s, "uname") == SSH_OK);
    bool is_err = false;
    int trc = 0;
    while ((trc = ssh_channel_read(s, (uint8_t *)out, sizeof(out), &is_err)) == 0) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
    assert(trc == SSH_ERR_CRYPTO);
    ssh_close(s);
    close(fds[0]);
    pthread_join(th, NULL);

    printf("ssh_host: all tests passed\n");

    if (getenv("SSH_LOCAL_TEST")) {
        int sfd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(22) };
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(connect(sfd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
        ssh_cfg_t lcfg = {
            .host = "127.0.0.1", .port = 22, .user = "dsc",
            .password = "invalid", .timeout_ms = 5000, .hostkey_cb = accept_hk,
        };
        ssh_session_t *ls = NULL;
        int lrc = ssh_open_io(&ls, &lcfg, sock_recv, sock_send, &sfd);
        printf("local sshd rc=%d\n", lrc);
        if (ls) ssh_close(ls);
        close(sfd);
    }
    return 0;
}
