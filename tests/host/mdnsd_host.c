/*
 * The multicast DNS responder (net/mdnsd.c) on the host, under ASan/UBSan: the names it answers, the ones it
 * must not, unicast versus group replies, and then malformed and random packets.
 */
#include "net/mdnsd.h"
#include "net/ip.h"
#include "kernel/identity.h"
#include "shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *node_name(void) { return "pico-home"; }
static net_state_t g_state;
const net_state_t *net_state(void) { return &g_state; }
int udp_bind(uint16_t port, udp_recv_fn cb, void *ctx) { (void)port; (void)cb; (void)ctx; return 0; }
int udp_send(const uint8_t d[IPV4_LEN], uint16_t dp, uint16_t sp, const uint8_t *data, uint32_t len) {
    (void)d; (void)dp; (void)sp; (void)data; return (int)len;
}

static int g_bad;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); g_bad = 1; } } while (0)

static const uint8_t IP[4] = { 192, 168, 178, 171 };

static uint32_t query(uint8_t *b, const char *name, uint16_t type, uint16_t cls) {
    memset(b, 0, 12);
    b[5] = 1;
    uint32_t n = 12;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        b[n++] = (uint8_t)l; memcpy(b + n, p, l); n += (uint32_t)l;
        p += l + (dot ? 1 : 0);
    }
    b[n++] = 0;
    b[n++] = (uint8_t)(type >> 8); b[n++] = (uint8_t)type; b[n++] = (uint8_t)(cls >> 8); b[n++] = (uint8_t)cls;
    return n;
}

int main(void) {
    uint8_t q[256], out[MDNSD_MAX_REPLY];
    bool uni;

    uint32_t n = query(q, "pico-home.local", 1, 1);
    uint32_t r = mdnsd_handle(q, n, "pico-home", IP, out, sizeof out, &uni, 5353);
    CHECK(r > 0, "no answer for the node's own name");
    CHECK(r >= 4 && memcmp(out + r - 4, IP, 4) == 0, "the answer does not end with the address");
    CHECK(out[2] == 0x84 && out[7] == 1, "not an authoritative single answer");
    CHECK(!uni, "a group query on 5353 was answered by unicast");

    n = query(q, "Pico-Home.LOCAL", 255, 1);
    CHECK(mdnsd_handle(q, n, "pico-home", IP, out, sizeof out, &uni, 5353) > 0, "ANY / mixed case not answered");
    n = query(q, "pico-home.local", 1, 0x8001);
    CHECK(mdnsd_handle(q, n, "pico-home", IP, out, sizeof out, &uni, 5353) > 0 && uni, "the QU bit did not ask for unicast");
    n = query(q, "pico-home.local", 1, 1);
    CHECK(mdnsd_handle(q, n, "pico-home", IP, out, sizeof out, &uni, 40000) > 0 && uni, "a legacy querier was answered on the group");

    n = query(q, "other.local", 1, 1);
    CHECK(mdnsd_handle(q, n, "pico-home", IP, out, sizeof out, &uni, 5353) == 0, "answered another host's name");
    n = query(q, "pico-home.example", 1, 1);
    CHECK(mdnsd_handle(q, n, "pico-home", IP, out, sizeof out, &uni, 5353) == 0, "answered a name outside .local");
    n = query(q, "pico-home.local", 28, 1);
    CHECK(mdnsd_handle(q, n, "pico-home", IP, out, sizeof out, &uni, 5353) == 0, "answered AAAA with something");
    n = query(q, "pico-home.local", 1, 1);
    q[2] = 0x80;
    CHECK(mdnsd_handle(q, n, "pico-home", IP, out, sizeof out, &uni, 5353) == 0, "answered a response");

    CHECK(!mdnsd_label_ok("Lugal 1") && !mdnsd_label_ok("-x") && !mdnsd_label_ok("") && mdnsd_label_ok("Lugal1"), "label rules");
    n = query(q, "lugal 1.local", 1, 1);
    CHECK(mdnsd_handle(q, n, "lugal 1", IP, out, sizeof out, &uni, 5353) == 0, "answered an invalid host label");

    srand(7);
    for (int i = 0; i < 500000; i++) {
        uint32_t len = (uint32_t)(rand() % 120);
        for (uint32_t k = 0; k < len; k++) q[k] = (uint8_t)rand();
        if (len > 12 && (i & 1)) { q[2] &= 0x7f; q[4] = 0; q[5] = (uint8_t)(1 + rand() % 3); }
        (void)mdnsd_handle(q, len, "pico-home", IP, out, sizeof out, &uni, 5353);
    }
    if (!g_bad) puts("mdnsd_host: own name, case, ANY, QU and legacy unicast, foreign names refused, 500k hostile packets");
    return g_bad;
}
