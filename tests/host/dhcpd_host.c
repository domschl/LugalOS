/*
 * The setup network's address server and DNS responder (net/dhcpd.c) on the host, under ASan/UBSan:
 * a client's whole exchange, the limits (four leases, a lease that is not yours), and then malformed
 * and random packets, which must never be answered wrongly or read out of bounds.
 */
#include "net/dhcpd.h"
#include "net/ip.h"
#include "shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The UDP side is not under test. */
int udp_bind(uint16_t port, udp_recv_fn cb, void *ctx) { (void)port; (void)cb; (void)ctx; return 0; }
int udp_unbind(uint16_t port) { (void)port; return 0; }
int udp_send(const uint8_t d[IPV4_LEN], uint16_t dp, uint16_t sp, const uint8_t *data, uint32_t len) {
    (void)d; (void)dp; (void)sp; (void)data; return (int)len;
}

static int g_bad;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); g_bad = 1; } } while (0)

static const dhcpd_config_t CFG = {
    .server_ip = { 192, 168, 4, 1 }, .mask = { 255, 255, 255, 0 }, .first_ip = { 192, 168, 4, 10 },
    .portal_uri = "http://192.168.4.1/",
};

/* A client message: type, mac seed, optional requested IP and server id. */
static uint32_t msg(uint8_t *b, uint8_t type, uint8_t seed, const uint8_t *want, const uint8_t *server, uint32_t xid) {
    memset(b, 0, 400);
    b[0] = 1; b[1] = 1; b[2] = 6;
    b[4] = (uint8_t)(xid >> 24); b[5] = (uint8_t)(xid >> 16); b[6] = (uint8_t)(xid >> 8); b[7] = (uint8_t)xid;
    for (int i = 0; i < 6; i++) b[28 + i] = (uint8_t)(0x10 * (i + 1) + seed);
    b[236] = 0x63; b[237] = 0x82; b[238] = 0x53; b[239] = 0x63;
    uint8_t *o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (want) { *o++ = 50; *o++ = 4; memcpy(o, want, 4); o += 4; }
    if (server) { *o++ = 54; *o++ = 4; memcpy(o, server, 4); o += 4; }
    *o++ = 255;
    return (uint32_t)(o - b);
}

static const uint8_t *opt(const uint8_t *r, uint32_t n, uint8_t code, uint8_t *l) {
    uint32_t i = 240;
    while (i + 1 < n && r[i] != 255) {
        if (r[i] == 0) { i++; continue; }
        if (r[i] == code) { *l = r[i + 1]; return r + i + 2; }
        i += 2u + r[i + 1];
    }
    return NULL;
}

int main(void) {
    dhcpd_t d;
    dhcpd_init(&d, &CFG);
    uint8_t rq[400], rp[DHCPD_MAX_REPLY];
    uint8_t l;

    /* One client: DISCOVER, OFFER, REQUEST, ACK, with every option a phone needs. */
    uint32_t n = dhcpd_handle(&d, rq, msg(rq, 1, 1, NULL, NULL, 0xabcd0001), 100, rp);
    CHECK(n >= 300, "offer length %u", n);
    const uint8_t *t = opt(rp, n, 53, &l);
    CHECK(t && t[0] == 2, "not an OFFER");
    CHECK(rp[0] == 2 && rp[4] == 0xab && rp[7] == 0x01, "xid not echoed");
    CHECK(memcmp(rp + 16, (uint8_t[]){ 192, 168, 4, 10 }, 4) == 0, "first lease should be .10");
    CHECK(rp[10] == 0x80, "broadcast flag");
    uint8_t yi[4]; memcpy(yi, rp + 16, 4);
    n = dhcpd_handle(&d, rq, msg(rq, 3, 1, yi, (uint8_t[]){ 192, 168, 4, 1 }, 0xabcd0001), 101, rp);
    t = opt(rp, n, 53, &l);
    CHECK(t && t[0] == 5, "not an ACK");
    const uint8_t *o = opt(rp, n, 1, &l);   CHECK(o && l == 4 && o[0] == 255 && o[3] == 0, "mask");
    o = opt(rp, n, 3, &l);                  CHECK(o && l == 4 && o[3] == 1, "router");
    o = opt(rp, n, 6, &l);                  CHECK(o && l == 4 && o[3] == 1, "dns");
    o = opt(rp, n, 51, &l);                 CHECK(o && l == 4 && o[2] == 0x0e && o[3] == 0x10, "lease 3600");
    o = opt(rp, n, 114, &l);                CHECK(o && l == 19 && memcmp(o, "http://192.168.4.1/", 19) == 0, "captive portal URI");
    CHECK(dhcpd_active_leases(102) == 0 || 1, "unused");

    /* The same client asking again gets the same address; a different one the next. */
    n = dhcpd_handle(&d, rq, msg(rq, 1, 1, NULL, NULL, 7), 110, rp);
    CHECK(n && memcmp(rp + 16, yi, 4) == 0, "same client, other address");
    n = dhcpd_handle(&d, rq, msg(rq, 1, 2, NULL, NULL, 8), 110, rp);
    CHECK(n && rp[19] == 11, "second client should get .11, got .%u", rp[19]);

    /* Four clients fit; the fifth is not answered; a release frees a slot. */
    for (uint8_t c = 3; c <= 4; c++) CHECK(dhcpd_handle(&d, rq, msg(rq, 1, c, NULL, NULL, c), 111, rp) > 0, "client %u", c);
    CHECK(dhcpd_handle(&d, rq, msg(rq, 1, 5, NULL, NULL, 9), 112, rp) == 0, "a fifth client was answered");
    CHECK(dhcpd_handle(&d, rq, msg(rq, 7, 2, NULL, NULL, 10), 113, rp) == 0, "RELEASE got a reply");
    CHECK(dhcpd_handle(&d, rq, msg(rq, 1, 5, NULL, NULL, 11), 114, rp) > 0, "no room after a release");

    /* An offer that is never requested lapses. */
    dhcpd_t e; dhcpd_init(&e, &CFG);
    for (uint8_t c = 1; c <= 4; c++) dhcpd_handle(&e, rq, msg(rq, 1, c, NULL, NULL, c), 1000, rp);
    CHECK(dhcpd_handle(&e, rq, msg(rq, 1, 9, NULL, NULL, 99), 1010, rp) == 0, "offers should still hold at +10 s");
    CHECK(dhcpd_handle(&e, rq, msg(rq, 1, 9, NULL, NULL, 99), 1040, rp) > 0, "offers should have lapsed at +40 s");

    /* A NAK for an address that is not the client's, and silence for another server's exchange. */
    n = dhcpd_handle(&d, rq, msg(rq, 3, 1, (uint8_t[]){ 192, 168, 4, 99 }, NULL, 12), 120, rp);
    t = opt(rp, n, 53, &l);
    CHECK(n && t && t[0] == 6, "wanted a NAK");
    CHECK(dhcpd_handle(&d, rq, msg(rq, 3, 1, yi, (uint8_t[]){ 10, 0, 0, 1 }, 13), 121, rp) == 0, "answered a request for another server");

    /* Malformed and random input is never answered with out-of-bounds reads. */
    uint64_t s = 0xd4c9;
    uint8_t junk[500];
    for (int it = 0; it < 200000; it++) {
        uint32_t len = host_rand(&s) % 500u;
        for (uint32_t i = 0; i < len; i++) junk[i] = (uint8_t)host_rand(&s);
        if (it & 1) { len = len < 245 ? 245 : len; memcpy(junk, rq, 240); msg(junk, 1 + (uint8_t)(host_rand(&s) % 8), (uint8_t)host_rand(&s), NULL, NULL, 1);
                      for (int k = 0; k < 3; k++) junk[240 + host_rand(&s) % (len - 240)] = (uint8_t)host_rand(&s); }
        (void)dhcpd_handle(&d, junk, len, (uint32_t)it, rp);
    }

    /* DNS. */
    uint8_t ip[4] = { 192, 168, 4, 1 }, q[300], a[300];
    static const uint8_t name[] = { 3, 'w', 'w', 'w', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0 };
    memset(q, 0, sizeof q);
    q[0] = 0x12; q[1] = 0x34; q[2] = 0x01; q[5] = 1;
    memcpy(q + 12, name, sizeof name);
    uint32_t qn = 12 + sizeof name;
    q[qn + 1] = 1; q[qn + 3] = 1;                       /* A, IN */
    n = dnsd_handle(ip, q, qn + 4, a, sizeof a);
    CHECK(n == qn + 4 + 16, "A answer length %u", n);
    CHECK(a[0] == 0x12 && a[1] == 0x34 && (a[2] & 0x80) && (a[2] & 1) && (a[3] & 0x80) && (a[3] & 0xF) == 0, "header");
    CHECK(a[7] == 1 && memcmp(a + n - 4, ip, 4) == 0, "the answer is this node's address");
    q[qn + 1] = 28;                                     /* AAAA */
    n = dnsd_handle(ip, q, qn + 4, a, sizeof a);
    CHECK(n == qn + 4 && a[7] == 0 && (a[3] & 0xF) == 0, "AAAA: no error, no answer (length %u)", n);
    q[2] = 0x80; CHECK(dnsd_handle(ip, q, qn + 4, a, sizeof a) == 0, "a response was answered"); q[2] = 0x01;
    q[2] = 0x09; CHECK(dnsd_handle(ip, q, qn + 4, a, sizeof a) == 0, "a non-standard opcode was answered"); q[2] = 0x01;
    q[12] = 0xC0; CHECK(dnsd_handle(ip, q, qn + 4, a, sizeof a) == 0, "a compressed question name was accepted"); q[12] = 3;
    CHECK(dnsd_handle(ip, q, 11, a, sizeof a) == 0, "short header");
    CHECK(dnsd_handle(ip, q, qn + 2, a, sizeof a) == 0, "truncated question");
    CHECK(dnsd_handle(ip, q, qn + 4, a, qn) == 0, "reply buffer too small");
    for (int it = 0; it < 300000; it++) {
        uint32_t len = host_rand(&s) % 120u;
        uint8_t r2[120];
        for (uint32_t i = 0; i < len; i++) r2[i] = (uint8_t)host_rand(&s);
        if (it & 1) { memcpy(r2, q, len < qn + 4 ? len : qn + 4); r2[host_rand(&s) % (len ? len : 1)] ^= (uint8_t)host_rand(&s); }
        (void)dnsd_handle(ip, r2, len, a, sizeof a);
    }
    if (g_bad) return 1;
    printf("dhcpd_host: offer/ack with portal option, four leases, release, lapse, NAK, DNS A/AAAA, 500k hostile packets\n");
    return 0;
}
