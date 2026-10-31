/* Host test of net/tls.c, the TLS 1.3 server.
 *
 * Interop, against OpenSSL's s_client over a real loopback socket (skipped if there is no `openssl`):
 * a full handshake with certificate verification and data both ways; a client whose first key share
 * is P-256, which must get a HelloRetryRequest and finish on X25519; a KeyUpdate the client asks to
 * be answered; and clients the server must refuse (AES-GCM only, P-256 only, TLS 1.2 only).
 *
 * Fuzz, in memory: a well-formed ClientHello built here, every single-bit change and every truncation
 * of it, and random records, fed through tls_read() (ASan/UBSan and valgrind watch for a read outside
 * a buffer; the server must answer each with an alert or carry on, never crash). */
#define _GNU_SOURCE
#include "net/tls.h"
#include "kernel/crypto.h"
#include "kernel/der.h"
#include "shim.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

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

/* The certificate der_host.c reads (CN=hosttest.local, SAN hosttest.local, from "Host Test CA"), its
 * private key, and the CA's certificate for the client to verify against. */
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
static const char KEY_HEX[] = "4c5a2af02ddabd5312131c44a87e7a4c11a96652e083acb47b947d86525d6f2a";
static const char CA_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBhDCCASmgAwIBAgIUcUugAsSA+RnwfdaOo8c178Z2uUowCgYIKoZIzj0EAwIw\n"
    "FzEVMBMGA1UEAwwMSG9zdCBUZXN0IENBMB4XDTI2MTAzMTEzMzM0N1oXDTM2MTAy\n"
    "ODEzMzM0N1owFzEVMBMGA1UEAwwMSG9zdCBUZXN0IENBMFkwEwYHKoZIzj0CAQYI\n"
    "KoZIzj0DAQcDQgAE5Ii+UEbkBSN9uTxqov4yjp+cPlHJ4DP+AF1v5aq7A5215Cg9\n"
    "Zs0/PQvkGR9MKqm+fonCEv/gihIofOsehrmj0KNTMFEwHQYDVR0OBBYEFLAQ3nCE\n"
    "70Kv4Icn8CY/14uspUj3MB8GA1UdIwQYMBaAFLAQ3nCE70Kv4Icn8CY/14uspUj3\n"
    "MA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDSQAwRgIhANm6wOJgeJkSTx7c\n"
    "/OJIIuFSYcaUMAxnYisJfdXAK9J/AiEAj/012rlXWApHZs79Fc9qO81W2qLBN1RV\n"
    "gaMDut51knQ=\n"
    "-----END CERTIFICATE-----\n";

static uint8_t g_cert[1024], g_key[32];
static uint32_t g_cert_len;
static uint8_t g_rx[TLS_RECORD_MAX], g_tx[TLS_TX_CAP];

/* --- over a socket ---------------------------------------------------------------------------- */

static int sock_recv(void *io, uint8_t *buf, uint32_t len) {
    ssize_t n = recv(*(int *)io, buf, len, MSG_DONTWAIT);
    if (n > 0) return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;
}
static int sock_send(void *io, const uint8_t *buf, uint32_t len) {
    ssize_t n = send(*(int *)io, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n >= 0) return (int)n;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}

typedef struct {
    bool established;
    int  result;                 /* the last tls_read() result that ended the session */
    char got[256];               /* what the client sent, as text */
    uint8_t alert;
} serve_out_t;

/* One connection: handshake, then echo each line back as "echo:<line>", until the client goes. */
static void serve(int c, serve_out_t *out) {
    tls_t t;
    memset(out, 0, sizeof *out);
    tls_server_init(&t, g_cert, g_cert_len, g_key, sock_recv, sock_send, &c, g_rx, g_tx);
    uint8_t buf[2048];
    size_t got = 0;
    for (int spins = 0; spins < 2000; spins++) {
        int n = tls_read(&t, buf, sizeof buf);
        if (tls_established(&t)) out->established = true;
        if (n > 0) {
            if (got + (size_t)n < sizeof out->got) { memcpy(out->got + got, buf, (size_t)n); got += (size_t)n; }
            char reply[300];
            int k = snprintf(reply, sizeof reply, "echo:%.*s", n, (const char *)buf);
            for (int off = 0; off < k;) {
                int w = tls_write(&t, (const uint8_t *)reply + off, (uint32_t)(k - off));
                if (w < 0) break;
                off += w;
                if (w == 0) usleep(1000);
            }
            continue;
        }
        if (n < 0) { out->result = n; break; }
        struct pollfd p = { c, POLLIN, 0 };
        (void)poll(&p, 1, 10);
    }
    out->alert = t.alert;
    while (!tls_flush(&t)) usleep(1000);
    tls_close(&t);
    tls_wipe(&t);
}

/* Runs `openssl s_client ARGS` against a fresh listener, its input from the shell commands `input`;
 * returns its exit status, its output in `out`. */
static int run_client(const char *args, const char *input, serve_out_t *srv, char *out, size_t cap) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 1) != 0) return -1;
    getsockname(ls, (struct sockaddr *)&a, &al);
    char cmd[1024], tmp[] = "/tmp/tls_host_XXXXXX";
    int fd = mkstemp(tmp);
    close(fd);
    snprintf(cmd, sizeof cmd,
             "(%s; sleep 1) | openssl s_client -connect 127.0.0.1:%u -servername hosttest.local "
             "-CAfile /tmp/tls_host_ca.pem -verify_return_error -no_ign_eof %s > %s 2>&1",
             input, ntohs(a.sin_port), args, tmp);
    pid_t pid = fork();
    if (pid == 0) { execl("/bin/sh", "sh", "-c", cmd, (char *)NULL); _exit(127); }
    struct pollfd p = { ls, POLLIN, 0 };
    if (poll(&p, 1, 10000) == 1) {
        int c = accept(ls, NULL, NULL);
        serve(c, srv);
        close(c);
    }
    close(ls);
    int st = 0;
    waitpid(pid, &st, 0);
    FILE *f = fopen(tmp, "r");
    size_t n = f ? fread(out, 1, cap - 1, f) : 0;
    out[n] = 0;
    if (f) fclose(f);
    unlink(tmp);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void test_interop(void) {
    if (system("command -v openssl >/dev/null 2>&1") != 0) { printf("tls_host: no openssl, interop skipped\n"); return; }
    FILE *f = fopen("/tmp/tls_host_ca.pem", "w");
    fputs(CA_PEM, f);
    fclose(f);
    static char out[16384];
    serve_out_t s;

    run_client("-tls1_3", "printf 'hello\\n'", &s, out, sizeof out);
    CHECK(s.established, "the handshake completes");
    CHECK(strstr(out, "Verify return code: 0 (ok)") != NULL, "the client verifies the certificate");
    CHECK(strstr(out, "TLS_CHACHA20_POLY1305_SHA256") != NULL, "on ChaCha20-Poly1305");
    CHECK(strstr(out, "X25519") != NULL, "with X25519");
    CHECK(strstr(s.got, "hello") != NULL && strstr(out, "echo:hello") != NULL, "data both ways (server got '%s')", s.got);

    run_client("-tls1_3 -groups P-256:X25519", "printf 'retry\\n'", &s, out, sizeof out);
    CHECK(s.established && strstr(out, "echo:retry") != NULL, "a P-256 first share gets a HelloRetryRequest and finishes");

    run_client("-tls1_3", "printf 'one\\n'; sleep 0.5; printf 'K\\n'; sleep 0.5; printf 'two\\n'", &s, out, sizeof out);
    CHECK(s.established && strstr(out, "echo:one") && strstr(out, "echo:two") && strstr(out, "KEYUPDATE"),
          "a KeyUpdate with update_requested, then more data");
    if (!(strstr(out, "echo:two"))) printf("---- s_client said:\n%s\n----\n", out + (strlen(out) > 1500 ? strlen(out) - 1500 : 0));

    run_client("-tls1_3 -ciphersuites TLS_AES_128_GCM_SHA256", "printf 'x\\n'", &s, out, sizeof out);
    CHECK(!s.established && s.alert == 40, "AES-GCM only is refused (alert %u)", s.alert);
    run_client("-tls1_3 -groups P-256", "printf 'x\\n'", &s, out, sizeof out);
    CHECK(!s.established && s.alert == 40, "P-256 only is refused (alert %u)", s.alert);
    run_client("-tls1_2", "printf 'x\\n'", &s, out, sizeof out);
    CHECK(!s.established && s.alert == 70, "TLS 1.2 only is refused (alert %u)", s.alert);
    unlink("/tmp/tls_host_ca.pem");
}

/* --- in memory ------------------------------------------------------------------------------- */

typedef struct { const uint8_t *in; uint32_t len, off; uint32_t sent; } mem_io_t;
static int mem_recv(void *io, uint8_t *buf, uint32_t len) {
    mem_io_t *m = (mem_io_t *)io;
    if (m->off >= m->len) return -1;
    uint32_t k = m->len - m->off < len ? m->len - m->off : len;
    memcpy(buf, m->in + m->off, k);
    m->off += k;
    return (int)k;
}
static int mem_send(void *io, const uint8_t *buf, uint32_t len) { (void)buf; ((mem_io_t *)io)->sent += len; return (int)len; }

/* A ClientHello record: TLS 1.3, ChaCha20, ECDSA P-256, an X25519 share (or none, to draw an HRR). */
static uint32_t make_client_hello(uint8_t *rec, bool with_share) {
    uint8_t *m = rec + 5;
    uint32_t o = 4;
    m[o++] = 3; m[o++] = 3;
    for (int i = 0; i < 32; i++) m[o++] = (uint8_t)next();
    m[o++] = 32; for (int i = 0; i < 32; i++) m[o++] = (uint8_t)i;
    m[o++] = 0; m[o++] = 4; m[o++] = 0x13; m[o++] = 0x01; m[o++] = 0x13; m[o++] = 0x03;
    m[o++] = 1; m[o++] = 0;
    uint32_t ext = o; o += 2;
    static const uint8_t sv[] = { 0, 43, 0, 3, 2, 3, 4 };
    static const uint8_t sa[] = { 0, 13, 0, 4, 0, 2, 4, 3 };
    static const uint8_t sg[] = { 0, 10, 0, 6, 0, 4, 0, 0x1d, 0, 0x17 };
    memcpy(m + o, sv, sizeof sv); o += sizeof sv;
    memcpy(m + o, sa, sizeof sa); o += sizeof sa;
    memcpy(m + o, sg, sizeof sg); o += sizeof sg;
    if (with_share) {
        static const uint8_t ks[] = { 0, 51, 0, 38, 0, 36, 0, 0x1d, 0, 32 };
        memcpy(m + o, ks, sizeof ks); o += sizeof ks;
        uint8_t sec[32];
        x25519_keypair(sec, m + o);
        o += 32;
    }
    m[ext] = (uint8_t)((o - ext - 2) >> 8); m[ext + 1] = (uint8_t)(o - ext - 2);
    m[0] = 1; m[1] = 0; m[2] = (uint8_t)((o - 4) >> 8); m[3] = (uint8_t)(o - 4);
    rec[0] = 22; rec[1] = 3; rec[2] = 1; rec[3] = (uint8_t)(o >> 8); rec[4] = (uint8_t)o;
    return 5 + o;
}

static int feed(const uint8_t *in, uint32_t len, uint32_t *sent) {
    tls_t t;
    mem_io_t io = { in, len, 0, 0 };
    tls_server_init(&t, g_cert, g_cert_len, g_key, mem_recv, mem_send, &io, g_rx, g_tx);
    int r = 0;
    uint8_t buf[512];
    for (int i = 0; i < 8 && r >= 0; i++) r = tls_read(&t, buf, sizeof buf);
    if (sent) *sent = io.sent;
    tls_wipe(&t);
    return r;
}

static void test_fuzz(unsigned iters) {
    uint8_t ch[600], m[700];
    uint32_t sent = 0;
    uint32_t n = make_client_hello(ch, true);
    int r = feed(ch, n, &sent);
    CHECK(r == TLS_EOF && sent > 600, "a well-formed ClientHello draws the whole server flight (%u bytes)", sent);
    uint32_t n2 = make_client_hello(m, false);
    (void)feed(m, n2, &sent);
    CHECK(sent > 80 && sent < 200, "no key share draws a HelloRetryRequest (%u bytes)", sent);

    for (uint32_t i = 0; i < n; i++) {
        for (int bit = 0; bit < 8; bit++) {
            memcpy(m, ch, n);
            m[i] ^= (uint8_t)(1u << bit);
            (void)feed(m, n, NULL);
        }
        (void)feed(ch, i, NULL);
    }
    for (unsigned it = 0; it < iters; it++) {
        uint32_t len = (uint32_t)(next() % sizeof m);
        for (uint32_t i = 0; i < len; i++) m[i] = (uint8_t)next();
        if (len >= 5 && (next() & 1)) {                  /* a plausible record header in front */
            m[0] = (uint8_t)(20 + next() % 4); m[1] = 3; m[2] = 3;
            m[3] = (uint8_t)((len - 5) >> 8); m[4] = (uint8_t)(len - 5);
        }
        (void)feed(m, len, NULL);
    }
}

int main(int argc, char **argv) {
    unsigned iters = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 2000;
    g_state = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x40c0ffee;
    int n = pem_decode(CERT_PEM, sizeof CERT_PEM - 1, "CERTIFICATE", g_cert, sizeof g_cert, NULL);
    if (n <= 0) { printf("tls_host: the test certificate does not decode\n"); return 1; }
    g_cert_len = (uint32_t)n;
    for (int i = 0; i < 32; i++) { unsigned v; sscanf(KEY_HEX + 2 * i, "%2x", &v); g_key[i] = (uint8_t)v; }
    signal(SIGPIPE, SIG_IGN);
    test_fuzz(iters);
    test_interop();
    if (fails) { printf("tls_host: %d failure(s)\n", fails); return 1; }
    printf("tls_host: handshake, HRR, KeyUpdate and refusals against OpenSSL; %u fuzzed records: ok\n", iters);
    return 0;
}
